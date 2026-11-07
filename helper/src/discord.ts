/*
 * Discord sink: logs in with the account token, joins the voice channel,
 * starts a Go Live stream and sends OBS's already-encoded frames to it.
 *
 * Path A from GUIDELINE.md §5.2: frames go straight to the library's WebRTC
 * connection (sendVideoFrame / sendAudioFrame), no ffmpeg and no re-encode.
 * The library handles DAVE end-to-end encryption, RTP packetization and the
 * SPS VUI rewrite.
 */

import { readFile } from "node:fs/promises";

import { Streamer } from "@dank074/discord-video-stream";
import { Client } from "@lng2004/discord.js-selfbot-v13";

import { fail, log, ready, stats } from "./ipc.ts";
import type { FrameSink } from "./ipc.ts";
import { ReadyWatchdog, waitUntilReady } from "./connwatch.ts";
import { FrameTimer, Pacer } from "./pacing.ts";
import { type Config, type Frame, FrameType } from "./protocol.ts";

const LOGIN_TIMEOUT_MS = 15_000;
const JOIN_TIMEOUT_MS = 10_000;
const STREAM_TIMEOUT_MS = 10_000;
const MEDIA_TIMEOUT_MS = 15_000;
/** How long the media connection may stay down before the stream counts as lost. */
const MEDIA_GRACE_MS = 5_000;
const STATS_INTERVAL_MS = 30_000;
const OPUS_FRAME_MS = 20;

/** Minimal shape of the library's WebRTC wrapper that this sink uses. */
interface MediaConn {
	/** True only while the WebRTC connection is "connected". */
	readonly ready: boolean;
	readonly webRtcConn?: { state(): string };
	setPacketizer(codec: string): void;
	sendVideoFrame(frame: Buffer, frametimeMs: number): void;
	sendAudioFrame(frame: Buffer, frametimeMs: number): void;
	mediaConnection: {
		setSpeaking(speaking: boolean): void;
		setVideoAttributes(enabled: true, attr: { width: number; height: number; fps: number }): void;
	};
}

export function withTimeout<T>(promise: Promise<T>, ms: number, what: string): Promise<T> {
	let timer: NodeJS.Timeout;
	const timeout = new Promise<never>((_, reject) => {
		timer = setTimeout(() => reject(new Error(`${what} timed out after ${ms / 1000}s`)), ms);
	});
	return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

/** Gateway close codes worth explaining to a user. */
const CLOSE_REASONS: Record<number, string> = {
	4004: "the token was rejected; check the account token",
	4010: "invalid shard",
	4011: "sharding required",
	4013: "invalid intents",
	4014: "disallowed intents",
};

export function describeClose(code: number | undefined): string {
	const reason = code !== undefined ? CLOSE_REASONS[code] : undefined;
	return reason ? `${reason} (gateway code ${code})` : `gateway closed (code ${code ?? "?"})`;
}

function message(err: unknown): string {
	return err instanceof Error ? err.message : String(err);
}

export async function readToken(path: string): Promise<string> {
	let token: string;
	try {
		token = (await readFile(path, "utf8")).trim();
	} catch (err) {
		throw new Error(`cannot read the token file: ${(err as NodeJS.ErrnoException).code ?? message(err)}`);
	}
	if (!token) throw new Error("the token file is empty");
	if (/\s/.test(token)) throw new Error("the token file must contain only the token, on one line");
	return token;
}

export class DiscordSink implements FrameSink {
	private client?: Client;
	private streamer?: Streamer;
	private conn?: MediaConn;
	private pacer?: Pacer;
	private videoTimer?: FrameTimer;
	private readonly audioTimer = new FrameTimer(OPUS_FRAME_MS);
	private watchdog?: ReadyWatchdog;
	private statsTimer?: NodeJS.Timeout;
	private stopping = false;

	private sent = { video: 0, audio: 0, videoBytes: 0, notReady: 0 };

	async onConfig(config: Config): Promise<void> {
		if (config.video.codec !== "H264") fail(`unsupported video codec ${config.video.codec}`);
		if (config.audio.codec !== "opus") fail(`unsupported audio codec ${config.audio.codec}`);

		let token: string;
		try {
			token = await readToken(config.tokenFile);
		} catch (err) {
			fail(message(err));
		}

		const client = new Client();
		this.client = client;
		this.streamer = new Streamer(client);

		// A gateway close during login is a login failure; after it, a lost connection.
		let loggedIn = false;
		client.on("invalidated", () => this.lost("Discord invalidated the session"));
		client.on("shardDisconnect", (event: { code?: number }) => {
			if (!loggedIn) fail(`could not log in: ${describeClose(event?.code)}`);
			this.lost(`disconnected from Discord: ${describeClose(event?.code)}`);
		});

		log("logging in to Discord");
		try {
			await withTimeout(client.login(token), LOGIN_TIMEOUT_MS, "login");
		} catch (err) {
			fail(`could not log in: ${message(err)}`);
		}
		loggedIn = true;
		log(`logged in as ${client.user?.username ?? "unknown user"}`);

		try {
			await withTimeout(
				this.streamer.joinVoice(config.guildId, config.channelId),
				JOIN_TIMEOUT_MS,
				"joining the voice channel",
			);
		} catch (err) {
			fail(`could not join voice channel ${config.channelId}: ${message(err)}`);
		}
		log("joined the voice channel");

		try {
			this.conn = (await withTimeout(
				this.streamer.createStream(),
				STREAM_TIMEOUT_MS,
				"starting Go Live",
			)) as unknown as MediaConn;
		} catch (err) {
			fail(`could not start Go Live: ${message(err)}`);
		}

		const { width, height } = config.video;
		const fps = Math.round(config.video.fps) || 30;
		this.conn.setPacketizer("H264");
		this.conn.mediaConnection.setSpeaking(true);
		this.conn.mediaConnection.setVideoAttributes(true, { width, height, fps });
		this.videoTimer = new FrameTimer(1000 / fps);

		// createStream() returns before WebRTC is up; frames sent now would be dropped.
		const conn = this.conn;
		log("connecting the video connection");
		try {
			await waitUntilReady(() => conn.ready, MEDIA_TIMEOUT_MS);
		} catch (err) {
			fail(`could not connect the video to Discord (WebRTC ${this.rtcState()}): ${message(err)}`);
		}
		log("video connection is up");

		this.pacer = new Pacer((frame) => this.send(frame));

		// A short drop can recover by itself; only give up if it stays down.
		this.watchdog = new ReadyWatchdog(
			() => conn.ready,
			(downMs) =>
				this.lost(
					`lost the video connection to Discord (WebRTC ${this.rtcState()} for ${Math.round(downMs / 1000)}s)`,
				),
			MEDIA_GRACE_MS,
		);

		let lastBytes = 0;
		this.statsTimer = setInterval(() => {
			const kbps = Math.round(((this.sent.videoBytes - lastBytes) * 8) / STATS_INTERVAL_MS);
			lastBytes = this.sent.videoBytes;
			stats({
				videoKbps: kbps,
				...this.sent,
				queued: this.pacer?.queued ?? 0,
				resyncs: this.pacer?.resyncs ?? 0,
				recoveries: this.watchdog?.recoveries ?? 0,
			});
		}, STATS_INTERVAL_MS);
		this.statsTimer.unref();

		log(`streaming ${width}x${height}@${fps} to Go Live`);
		ready();
	}

	onVideo(frame: Frame): void {
		this.pacer?.push(frame);
	}

	onAudio(frame: Frame): void {
		this.pacer?.push(frame);
	}

	async onStop(): Promise<void> {
		this.stopping = true;
		this.watchdog?.stop();
		clearInterval(this.statsTimer);
		this.pacer?.stop();

		try {
			this.streamer?.stopStream();
			this.streamer?.leaveVoice();
		} catch (err) {
			log(`error while leaving: ${message(err)}`);
		}
		// Give the leave opcodes a moment to reach Discord before closing the gateway.
		await new Promise((resolve) => setTimeout(resolve, 300));
		await this.client?.destroy();

		log(`stopped after sending ${this.sent.video} video and ${this.sent.audio} audio frames`);
	}

	private rtcState(): string {
		try {
			return this.conn?.webRtcConn?.state() ?? "not created";
		} catch {
			return "unknown";
		}
	}

	private send(frame: Frame): void {
		const conn = this.conn;
		if (!conn) return;
		if (!conn.ready) {
			this.sent.notReady++;
			return;
		}

		try {
			if (frame.type === FrameType.VIDEO) {
				conn.sendVideoFrame(frame.payload, this.videoTimer?.next(frame.ptsUs) ?? 33);
				this.sent.video++;
				this.sent.videoBytes += frame.payload.length;
			} else {
				conn.sendAudioFrame(frame.payload, this.audioTimer.next(frame.ptsUs));
				this.sent.audio++;
			}
		} catch (err) {
			this.lost(`sending a frame failed: ${message(err)}`);
		}
	}

	private lost(reason: string): void {
		if (this.stopping) return;
		fail(reason);
	}
}
