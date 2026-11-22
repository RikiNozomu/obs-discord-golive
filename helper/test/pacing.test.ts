/* Unit tests for real-time pacing and token-file handling. Run: node --test test/ */

import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { type Clock, FrameTimer, Pacer } from "../src/pacing.ts";
import { type Frame, FrameType } from "../src/protocol.ts";

/** Deterministic clock: timers fire only when advance() passes them. */
class FakeClock implements Clock {
	t = 0;
	private timers = new Map<number, { at: number; fn: () => void }>();
	private nextId = 1;

	now(): number {
		return this.t;
	}
	setTimeout(fn: () => void, ms: number): unknown {
		const id = this.nextId++;
		this.timers.set(id, { at: this.t + ms, fn });
		return id;
	}
	clearTimeout(handle: unknown): void {
		this.timers.delete(handle as number);
	}
	advance(ms: number): void {
		const end = this.t + ms;
		for (;;) {
			let next: [number, { at: number; fn: () => void }] | undefined;
			for (const entry of this.timers) {
				if (entry[1].at <= end && (!next || entry[1].at < next[1].at)) next = entry;
			}
			if (!next) break;
			this.timers.delete(next[0]);
			this.t = Math.max(this.t, next[1].at);
			next[1].fn();
		}
		this.t = end;
	}
}

const video = (ptsMs: number): Frame => ({
	type: FrameType.VIDEO,
	flags: 0,
	ptsUs: ptsMs * 1000,
	payload: Buffer.alloc(0),
});
const audio = (ptsMs: number): Frame => ({ ...video(ptsMs), type: FrameType.AUDIO });

function setup(bufferMs = 80) {
	const clock = new FakeClock();
	const sent: { ptsMs: number; at: number; type: number }[] = [];
	const pacer = new Pacer(
		(f) => sent.push({ ptsMs: f.ptsUs / 1000, at: clock.now(), type: f.type }),
		clock,
		{ bufferMs, windowMs: 2000 },
	);
	return { clock, sent, pacer };
}

test("a burst of frames is spread out to real time", () => {
	const { clock, sent, pacer } = setup(80);
	// Three video frames arrive at once.
	pacer.push(video(0));
	pacer.push(video(33));
	pacer.push(video(66));

	clock.advance(13);
	assert.equal(sent.length, 0, "nothing before the jitter buffer elapses");
	clock.advance(200);
	assert.deepEqual(
		sent.map((s) => s.at),
		// Frame 66 arrived earliest relative to its pts, so it sets the timeline:
		// it waits the full 80 ms and the others keep their 33 ms spacing.
		[14, 47, 80],
	);
});

test("audio and video are sent in pts order on one clock", () => {
	const { clock, sent, pacer } = setup(0);
	pacer.push(video(0));
	pacer.push(video(40));
	pacer.push(audio(0));
	pacer.push(audio(20));
	clock.advance(100);
	assert.deepEqual(
		sent.map((s) => [s.type, s.ptsMs]),
		[
			[FrameType.VIDEO, 0],
			[FrameType.AUDIO, 0],
			[FrameType.AUDIO, 20],
			[FrameType.VIDEO, 40],
		],
	);
});

/** Pushes one frame at its own pts time (an on-time arrival) and runs the clock there. */
function pushAt(clock: FakeClock, pacer: Pacer, frame: Frame): void {
	const at = frame.ptsUs / 1000;
	if (at > clock.t) clock.advance(at - clock.t);
	pacer.push(frame);
}

test("an OBS stall does not add lasting delay (the frozen-video bug)", () => {
	const { clock, sent, pacer } = setup(80);
	// 1 s of normal 30 fps video.
	for (let pts = 0; pts <= 1000; pts += 33) pushAt(clock, pacer, video(pts));
	// OBS freezes for 2 s, then hands over the backlog in one burst.
	clock.advance(3000 - clock.t);
	for (let pts = 1023; pts < 3000; pts += 33) pacer.push(video(pts));
	clock.advance(1); // the backlog is late already: it goes out at once
	const backlog = sent.filter((s) => s.ptsMs >= 1023 && s.ptsMs < 3000);
	assert.ok(backlog.length > 50 && backlog.every((s) => s.at <= 3001), "backlog sent immediately");
	// Normal frames again: each must wait only the 80 ms buffer, not the 2 s stall.
	for (let pts = 3003; pts <= 4000; pts += 33) pushAt(clock, pacer, video(pts));
	clock.advance(200);
	const after = sent.filter((s) => s.ptsMs >= 3003);
	assert.ok(after.length > 25);
	for (const s of after) assert.ok(s.at - s.ptsMs <= 81, `pts ${s.ptsMs} waited ${s.at - s.ptsMs} ms`);
	assert.ok(pacer.lateFrames > 50);
});

test("repeated stalls never pile up delay", () => {
	const { clock, sent, pacer } = setup(80);
	let pts = 0;
	for (let round = 0; round < 10; round++) {
		for (let i = 0; i < 30; i++, pts += 33) pushAt(clock, pacer, video(pts));
		clock.advance(700); // stall
		for (let i = 0; i < 21; i++, pts += 33) pacer.push(video(pts)); // backlog
	}
	for (let i = 0; i < 30; i++, pts += 33) pushAt(clock, pacer, video(pts));
	clock.advance(200);
	const last = sent.at(-1);
	assert.ok(last && last.at - last.ptsMs <= 81, `final delay ${last && last.at - last.ptsMs} ms`);
	assert.ok(pacer.queued < 5, `queue drained (${pacer.queued})`);
});

test("a lasting increase in OBS delay is adopted after the window", () => {
	const { clock, sent, pacer } = setup(80);
	for (let pts = 0; pts <= 1000; pts += 20) pushAt(clock, pacer, audio(pts));
	// From now on every frame arrives 150 ms after its pts (more audio buffering in OBS).
	for (let pts = 1020; pts <= 8000; pts += 20) {
		clock.advance(pts + 150 - clock.t);
		pacer.push(audio(pts));
	}
	clock.advance(200);
	const tail = sent.filter((s) => s.ptsMs > 7000);
	assert.ok(tail.length > 40);
	for (const s of tail) {
		const wait = s.at - (s.ptsMs + 150);
		assert.ok(wait >= 79 && wait <= 81, `paced 80 ms over the new delay, got ${wait} ms`);
	}
});

test("stop discards queued frames and sends nothing more", () => {
	const { clock, sent, pacer } = setup(80);
	pacer.push(video(0));
	pacer.stop();
	pacer.push(video(33));
	clock.advance(1000);
	assert.equal(sent.length, 0);
});

test("FrameTimer passes pts deltas through, including gaps from dropped frames", () => {
	const t = new FrameTimer(33.333);
	assert.equal(t.next(0), 33.333, "first frame uses the nominal duration");
	assert.equal(t.next(33_333), 33.333);
	assert.equal(t.next(133_333), 100, "two dropped frames carry into the RTP clock");
	assert.equal(t.next(133_333), 33.333, "zero or negative deltas fall back to nominal");
});

test("readToken trims the file and rejects bad contents", async () => {
	const { readToken } = await import("../src/discord.ts");
	const dir = mkdtempSync(join(tmpdir(), "golive-token-"));
	const file = join(dir, "token.txt");

	writeFileSync(file, "  abc.def.ghi\n");
	assert.equal(await readToken(file), "abc.def.ghi");

	writeFileSync(file, "\n");
	await assert.rejects(readToken(file), /empty/);

	writeFileSync(file, "two words");
	await assert.rejects(readToken(file), /only the token/);

	await assert.rejects(readToken(join(dir, "missing")), /ENOENT/);
});

test("gateway close codes are explained", async () => {
	const { describeClose } = await import("../src/discord.ts");
	assert.match(describeClose(4004), /token was rejected/);
	assert.match(describeClose(1006), /code 1006/);
	assert.match(describeClose(undefined), /code \?/);
});

test("waitUntilReady waits for the connection and times out with a clear error", async () => {
	const { waitUntilReady } = await import("../src/connwatch.ts");
	const clock = new FakeClock();
	let ready = false;
	let done = false;
	void waitUntilReady(() => ready, 1000, clock).then(() => (done = true));
	clock.advance(500);
	await Promise.resolve();
	assert.equal(done, false, "not ready yet: keep waiting (this was the M3 bug)");
	ready = true;
	clock.advance(60);
	await Promise.resolve();
	assert.equal(done, true);

	const late = waitUntilReady(() => false, 1000, clock);
	clock.advance(1100);
	await assert.rejects(late, /not connected after 1s/);
});

test("ReadyWatchdog tolerates short drops and reports a long one once", async () => {
	const { ReadyWatchdog } = await import("../src/connwatch.ts");
	const clock = new FakeClock();
	let ready = true;
	const lost: number[] = [];
	const dog = new ReadyWatchdog(() => ready, (ms) => lost.push(ms), 5000, clock);

	clock.advance(1000);
	ready = false;
	clock.advance(3000); // down 3 s: within the grace period
	ready = true;
	clock.advance(500);
	assert.deepEqual(lost, []);
	assert.equal(dog.recoveries, 1);

	ready = false;
	clock.advance(6000);
	assert.equal(lost.length, 1);
	assert.ok(lost[0] >= 5000);
	clock.advance(10_000);
	assert.equal(lost.length, 1, "reported only once");
});
