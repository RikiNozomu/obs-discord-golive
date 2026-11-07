/*
 * Real-time pacing for frames headed to Discord (GUIDELINE.md §5.3).
 *
 * OBS hands frames over in bursts (encoder lookahead, interleaving, pipe
 * buffering). Sending a burst at once makes viewers stutter, so each frame is
 * held until wall-clock time  t0 + (pts - pts0) + buffer, with audio and video
 * on the same clock so they stay in sync.
 *
 * If frames arrive much later than their slot (OBS stalled, the machine was
 * busy), the clock is re-anchored instead of firing a backlog all at once.
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
	/** A frame this late re-anchors the clock. */
	resyncMs: number;
}

export const DEFAULT_PACER_OPTIONS: PacerOptions = { bufferMs: 80, resyncMs: 500 };

export class Pacer {
	private queue: Frame[] = [];
	private t0: number | null = null;
	private pts0 = 0;
	private timer: unknown = null;
	private stopped = false;

	private readonly send: (frame: Frame) => void;
	private readonly clock: Clock;
	private readonly opts: PacerOptions;

	resyncs = 0;

	constructor(send: (frame: Frame) => void, clock: Clock = realClock, opts: PacerOptions = DEFAULT_PACER_OPTIONS) {
		this.send = send;
		this.clock = clock;
		this.opts = opts;
	}

	push(frame: Frame): void {
		if (this.stopped) return;

		const now = this.clock.now();
		if (this.t0 === null) {
			this.anchor(frame.ptsUs, now);
		} else if (this.due(frame.ptsUs) < now - this.opts.resyncMs) {
			this.resyncs++;
			this.anchor(frame.ptsUs, now);
			// Frames already queued belong to the old timeline; send them now.
			this.flush();
		}

		// Keep the queue in pts order (audio and video interleave).
		let i = this.queue.length;
		while (i > 0 && this.queue[i - 1].ptsUs > frame.ptsUs) i--;
		this.queue.splice(i, 0, frame);

		this.schedule();
	}

	/** Sends everything still queued, immediately. */
	flush(): void {
		const pending = this.queue;
		this.queue = [];
		for (const f of pending) this.send(f);
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

	private anchor(ptsUs: number, now: number): void {
		this.pts0 = ptsUs;
		this.t0 = now + this.opts.bufferMs;
	}

	private due(ptsUs: number): number {
		return (this.t0 ?? 0) + (ptsUs - this.pts0) / 1000;
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
