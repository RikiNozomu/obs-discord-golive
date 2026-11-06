/*
 * Helper-side IPC: read frames from stdin, report status on stderr.
 *
 * stdout is left alone so it stays usable for piping media if a future
 * backend needs it. The plugin reads our stderr line by line:
 *   READY                 the helper is connected and accepting frames
 *   ERROR <message>       fatal; the helper exits non-zero after this
 *   LOG <message>         informational, forwarded to the OBS log
 *   STATS <json>          periodic counters
 */

import { type Config, FrameParser, type Frame, FrameType, parseConfig } from "./protocol.ts";

/** Discord bot/user tokens. Never let one reach a log. See GUIDELINE.md §6. */
const TOKEN_PATTERN = /[\w-]{24,}\.[\w-]{6}\.[\w-]{27,}/g;

export function redact(text: string): string {
	return text.replace(TOKEN_PATTERN, "<redacted-token>");
}

function emit(line: string): void {
	process.stderr.write(`${redact(line)}\n`);
}

export function ready(): void {
	emit("READY");
}

export function log(message: string): void {
	emit(`LOG ${message}`);
}

export function stats(payload: unknown): void {
	emit(`STATS ${JSON.stringify(payload)}`);
}

export function fail(message: string): never {
	emit(`ERROR ${message}`);
	process.exit(1);
}

export interface FrameSink {
	/** Called once, before any media frame. */
	onConfig(config: Config): Promise<void> | void;
	onVideo(frame: Frame): void;
	onAudio(frame: Frame): void;
	/** STOP frame or clean EOF. */
	onStop(): Promise<void> | void;
}

/**
 * Drive a sink from stdin until STOP or EOF.
 *
 * The first frame must be CONFIG; anything else is a protocol error. Frames
 * are handed over in arrival order, so a sink that needs pacing does it itself.
 */
export async function run(sink: FrameSink): Promise<void> {
	const parser = new FrameParser();
	let config: Config | undefined;
	let stopped = false;

	const handle = async (frame: Frame): Promise<void> => {
		if (stopped) return;

		if (!config) {
			if (frame.type !== FrameType.CONFIG) {
				fail(`expected a CONFIG frame first, got type ${frame.type}`);
			}
			config = parseConfig(frame.payload);
			await sink.onConfig(config);
			return;
		}

		switch (frame.type) {
			case FrameType.VIDEO:
				sink.onVideo(frame);
				break;
			case FrameType.AUDIO:
				sink.onAudio(frame);
				break;
			case FrameType.STOP:
				stopped = true;
				await sink.onStop();
				break;
			case FrameType.CONFIG:
				fail("received a second CONFIG frame");
				break;
		}
	};

	process.stdin.pause();

	for await (const chunk of process.stdin) {
		let frames: Frame[];
		try {
			frames = parser.push(chunk as Buffer);
		} catch (err) {
			fail((err as Error).message);
		}

		for (const frame of frames) {
			await handle(frame);
			if (stopped) return;
		}
	}

	// stdin closed. A partial frame means the plugin died mid-write.
	if (parser.pending > 0) {
		fail(`stream ended with ${parser.pending} bytes of an incomplete frame`);
	}
	if (!stopped) await sink.onStop();
}
