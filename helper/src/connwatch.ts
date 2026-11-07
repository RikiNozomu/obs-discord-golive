/*
 * Watching the WebRTC media connection to Discord's stream server.
 *
 * createStream() resolves when the stream server accepts the session, which
 * is *before* the WebRTC connection (ICE + DTLS) is up, so `ready` is still
 * false for a moment. Frames sent before then are thrown away by the library.
 * So: wait for `ready` first, and afterwards only treat the connection as
 * lost when it stays down for a while (a short "disconnected" can recover).
 */

import { type Clock, realClock } from "./pacing.ts";

const POLL_MS = 50;

/** Resolves once isReady() is true; rejects after timeoutMs. */
export function waitUntilReady(isReady: () => boolean, timeoutMs: number, clock: Clock = realClock): Promise<void> {
	return new Promise((resolve, reject) => {
		const start = clock.now();
		const check = () => {
			if (isReady()) return resolve();
			if (clock.now() - start >= timeoutMs) return reject(new Error(`not connected after ${timeoutMs / 1000}s`));
			clock.setTimeout(check, POLL_MS);
		};
		check();
	});
}

/** Calls onLost once the connection has been down for graceMs in a row. */
export class ReadyWatchdog {
	private downSince: number | null = null;
	private timer: unknown = null;
	private stopped = false;

	private readonly isReady: () => boolean;
	private readonly onLost: (downMs: number) => void;
	private readonly graceMs: number;
	private readonly clock: Clock;

	/** Times the connection dropped and came back by itself. */
	recoveries = 0;

	constructor(isReady: () => boolean, onLost: (downMs: number) => void, graceMs: number, clock: Clock = realClock) {
		this.isReady = isReady;
		this.onLost = onLost;
		this.graceMs = graceMs;
		this.clock = clock;
		this.schedule();
	}

	stop(): void {
		this.stopped = true;
		if (this.timer !== null) this.clock.clearTimeout(this.timer);
		this.timer = null;
	}

	private schedule(): void {
		if (this.stopped) return;
		this.timer = this.clock.setTimeout(() => this.tick(), 250);
		(this.timer as { unref?: () => void }).unref?.();
	}

	private tick(): void {
		const now = this.clock.now();
		if (this.isReady()) {
			if (this.downSince !== null) this.recoveries++;
			this.downSince = null;
		} else {
			this.downSince ??= now;
			if (now - this.downSince >= this.graceMs) {
				this.stop();
				this.onLost(now - this.downSince);
				return;
			}
		}
		this.schedule();
	}
}
