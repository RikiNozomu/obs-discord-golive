/*
 * Real-time pacing for frames headed to Discord (GUIDELINE.md §5.3).
 *
 * OBS hands frames over in bursts (encoder lookahead, interleaving, pipe
 * buffering). Sending a burst at once makes viewers stutter, so each frame is
 * held until
 *
 *     due = pts + minLag + buffer
 *
 * where lag = arrival time - pts, and minLag is the smallest lag seen in the
 * last few seconds: the delay of a frame that came through OBS with no hold-up.
 * Audio and video share it, so they stay in sync.
 *
 * When OBS stalls (encoder or render lag), the frames from the stall arrive
 * with a large lag. They do not move minLag, so they are already due and go
 * out at once, and the frames after them are paced exactly as before: a stall
 * costs one hiccup, never lasting delay. (The previous version re-anchored the
 * clock on the first late frame, which kept the stall's delay forever and let
 * repeated stalls pile up seconds of delay and periodic frozen video.)
 *
 * If OBS's delay grows for good (e.g. it adds audio buffering), minLag follows
 * once the old, smaller lags have left the window.
 */

import type { Frame } from "./protocol.ts";

export interface Clock {
	now(): number; // milliseconds
	setTimeout(fn: () => void, ms: number): unknown;
	clearTimeout(handle: unknown): void;
}

export const realClock: Clock = {
	now: () => performance.now(),
	setTimeout: (fn, ms) => setTimeout(fn, ms),
	clearTimeout: (h) => clearTimeout(h as NodeJS.Timeout),
};

export interface PacerOptions {
	/** Delay added to every frame to absorb arrival jitter. */
	bufferMs: number;
	/** How long the smallest lag is remembered (between 1x and 2x this). */
	windowMs: number;
}

export const DEFAULT_PACER_OPTIONS: PacerOptions = { bufferMs: 80, windowMs: 3000 };

/** A frame sent this much after its due time counts as late (stats only). */
const LATE_MS = 100;

export class Pacer {
	private queue: Frame[] = [];
	private timer: unknown = null;
	private stopped = false;

	/* Sliding minimum of lag over two buckets of windowMs each. */
	private bucketStart: number | null = null;
	private bucketMin = Infinity;
	private previousMin = Infinity;

	private readonly send: (frame: Frame) => void;
	private readonly clock: Clock;
	private readonly opts: PacerOptions;

	/** Frames that arrived too late to be paced (each one is an OBS hiccup). */
	lateFrames = 0;

	constructor(send: (frame: Frame) => void, clock: Clock = realClock, opts: PacerOptions = DEFAULT_PACER_OPTIONS) {
		this.send = send;
		this.clock = clock;
		this.opts = opts;
	}

	push(frame: Frame): void {
		if (this.stopped) return;

		const now = this.clock.now();
		this.observeLag(now - frame.ptsUs / 1000, now);
		if (this.due(frame.ptsUs) < now - LATE_MS) this.lateFrames++;

		// Keep the queue in pts order (audio and video interleave).
		let i = this.queue.length;
		while (i > 0 && this.queue[i - 1].ptsUs > frame.ptsUs) i--;
		this.queue.splice(i, 0, frame);

		this.schedule();
	}

	stop(): void {
		this.stopped = true;
		if (this.timer !== null) this.clock.clearTimeout(this.timer);
		this.timer = null;
		this.queue = [];
	}

	get queued(): number {
		return this.queue.length;
	}

	private observeLag(lag: number, now: number): void {
		if (this.bucketStart === null) this.bucketStart = now;
		if (now - this.bucketStart >= this.opts.windowMs) {
			// A whole bucket of silence means the old minimum is stale too.
			this.previousMin = now - this.bucketStart >= 2 * this.opts.windowMs ? Infinity : this.bucketMin;
			this.bucketMin = Infinity;
			this.bucketStart = now;
		}
		if (lag < this.bucketMin) this.bucketMin = lag;
	}

	private minLag(): number {
		return Math.min(this.bucketMin, this.previousMin);
	}

	private due(ptsUs: number): number {
		return ptsUs / 1000 + this.minLag() + this.opts.bufferMs;
	}

	private schedule(): void {
		if (this.timer !== null) this.clock.clearTimeout(this.timer);
		this.timer = null;
		if (this.stopped || this.queue.length === 0) return;

		const wait = Math.max(0, this.due(this.queue[0].ptsUs) - this.clock.now());
		this.timer = this.clock.setTimeout(() => this.tick(), wait);
	}

	private tick(): void {
		this.timer = null;
		const now = this.clock.now();
		while (this.queue.length > 0 && this.due(this.queue[0].ptsUs) <= now + 1) {
			const frame = this.queue.shift() as Frame;
			this.send(frame);
		}
		this.schedule();
	}
}

/**
 * Turns pts into the per-frame duration the RTP layer wants. The library adds
 * the duration *after* sending a frame, so passing (pts - previous pts) makes
 * every RTP timestamp equal the previous frame's pts: a constant one-frame
 * shift with no long-term drift, and gaps from dropped frames carry through.
 */
export class FrameTimer {
	private lastPtsUs: number | null = null;
	private readonly nominalMs: number;

	constructor(nominalMs: number) {
		this.nominalMs = nominalMs;
	}

	next(ptsUs: number): number {
		const ms = this.lastPtsUs === null ? this.nominalMs : (ptsUs - this.lastPtsUs) / 1000;
		this.lastPtsUs = ptsUs;
		return ms > 0 && ms < 5000 ? ms : this.nominalMs;
	}
}
