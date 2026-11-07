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
		{ bufferMs, resyncMs: 500 },
	);
	return { clock, sent, pacer };
}

test("a burst of frames is spread out to real time", () => {
	const { clock, sent, pacer } = setup(80);
	// Three video frames arrive at once.
	pacer.push(video(0));
	pacer.push(video(33));
	pacer.push(video(66));

	clock.advance(79);
	assert.equal(sent.length, 0, "nothing before the jitter buffer elapses");
	clock.advance(200);
	assert.deepEqual(
		sent.map((s) => s.at),
		[80, 113, 146],
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

test("frames arriving far too late re-anchor the clock instead of firing a backlog", () => {
	const { clock, sent, pacer } = setup(50);
	pacer.push(video(0));
	clock.advance(60);
	assert.equal(sent.length, 1);

	// OBS stalls for 2 s, then resumes with the next frame.
	clock.advance(2000);
	pacer.push(video(33));
	assert.equal(pacer.resyncs, 1);
	clock.advance(49);
	assert.equal(sent.length, 1, "re-anchored frame still gets the jitter buffer");
	clock.advance(2);
	assert.equal(sent.length, 2);
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
