/*
 * Mock sink: instead of talking to Discord, dump what the plugin sends and
 * check it for the mistakes that break real streaming.
 *
 * Writes into an output directory:
 *   video.h264   raw Annex-B stream, playable with ffplay / probe with ffprobe
 *   audio.opus   Ogg Opus, playable with ffplay
 *   report.json  config, per-stream counters, and the results of the checks
 *
 * This is how milestone M2 is verified without a Discord account.
 */

import { createWriteStream, mkdirSync, type WriteStream } from "node:fs";
import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { once } from "node:events";

import { type Config, type Frame, isKeyframe } from "./protocol.ts";
import type { FrameSink } from "./ipc.ts";
import { log, ready, stats } from "./ipc.ts";
import { OggOpusWriter } from "./ogg.ts";

const ANNEX_B_3 = Buffer.from([0x00, 0x00, 0x01]);
const ANNEX_B_4 = Buffer.from([0x00, 0x00, 0x00, 0x01]);

function startsWithStartCode(buf: Buffer): boolean {
	return buf.subarray(0, 4).equals(ANNEX_B_4) || buf.subarray(0, 3).equals(ANNEX_B_3);
}

interface StreamStats {
	frames: number;
	bytes: number;
	firstPtsUs: number | null;
	lastPtsUs: number | null;
	maxGapUs: number;
	nonMonotonic: number;
}

function newStats(): StreamStats {
	return { frames: 0, bytes: 0, firstPtsUs: null, lastPtsUs: null, maxGapUs: 0, nonMonotonic: 0 };
}

function record(s: StreamStats, frame: Frame): void {
	if (s.lastPtsUs !== null) {
		const gap = frame.ptsUs - s.lastPtsUs;
		if (gap < 0) s.nonMonotonic++;
		else if (gap > s.maxGapUs) s.maxGapUs = gap;
	}
	if (s.firstPtsUs === null) s.firstPtsUs = frame.ptsUs;
	s.lastPtsUs = frame.ptsUs;
	s.frames++;
	s.bytes += frame.payload.length;
}

function durationSec(s: StreamStats): number {
	if (s.firstPtsUs === null || s.lastPtsUs === null) return 0;
	return (s.lastPtsUs - s.firstPtsUs) / 1_000_000;
}

export class MockSink implements FrameSink {
	private config!: Config;
	private videoOut!: WriteStream;
	private audioOut!: WriteStream;
	private ogg!: OggOpusWriter;

	private video = newStats();
	private audio = newStats();
	private keyframes = 0;
	private problems: string[] = [];
	private checkedFirstVideo = false;
	private statsTimer?: NodeJS.Timeout;
	private readonly outDir: string;

	constructor(outDir: string) {
		this.outDir = outDir;
	}

	onConfig(config: Config): void {
		this.config = config;
		mkdirSync(this.outDir, { recursive: true });

		this.videoOut = createWriteStream(join(this.outDir, "video.h264"));
		this.audioOut = createWriteStream(join(this.outDir, "audio.opus"));
		this.ogg = new OggOpusWriter(this.audioOut, config.audio.channels, config.audio.sampleRate);

		log(
			`mock sink: ${config.video.width}x${config.video.height}@${config.video.fps} ` +
				`${config.video.codec}, ${config.audio.codec} ${config.audio.sampleRate}Hz ` +
				`x${config.audio.channels} -> ${this.outDir}`,
		);

		// Periodic counters, so a stalled stream is visible in the OBS log.
		this.statsTimer = setInterval(() => {
			stats({ video: this.video.frames, audio: this.audio.frames, keyframes: this.keyframes });
		}, 5000);
		this.statsTimer.unref();

		ready();
	}

	onVideo(frame: Frame): void {
		if (!this.checkedFirstVideo) {
			this.checkedFirstVideo = true;
			if (!isKeyframe(frame)) {
				this.problems.push("the first video frame is not a keyframe; late viewers would see nothing");
			}
			if (!startsWithStartCode(frame.payload)) {
				this.problems.push(
					"the first video frame does not begin with an Annex-B start code; " +
						"the encoder may be emitting AVCC instead",
				);
			}
		}
		if (isKeyframe(frame)) this.keyframes++;
		record(this.video, frame);
		this.videoOut.write(frame.payload);
	}

	onAudio(frame: Frame): void {
		record(this.audio, frame);
		this.ogg.write(frame.payload);
	}

	async onStop(): Promise<void> {
		if (this.statsTimer) clearInterval(this.statsTimer);
		this.ogg.end();

		this.videoOut.end();
		this.audioOut.end();
		await Promise.all([once(this.videoOut, "finish"), once(this.audioOut, "finish")]);

		const report = this.buildReport();
		await writeFile(join(this.outDir, "report.json"), `${JSON.stringify(report, null, 2)}\n`, "utf8");

		log(
			`wrote ${this.video.frames} video and ${this.audio.frames} audio frames ` +
				`(${report.checks.passed ? "checks passed" : `${this.problems.length} problem(s)`})`,
		);
	}

	private buildReport() {
		const videoSec = durationSec(this.video);
		const audioSec = durationSec(this.audio);

		if (this.video.frames === 0) this.problems.push("no video frames arrived");
		if (this.audio.frames === 0) this.problems.push("no audio frames arrived");
		if (this.video.nonMonotonic > 0) {
			this.problems.push(`${this.video.nonMonotonic} video timestamp(s) went backwards`);
		}
		if (this.audio.nonMonotonic > 0) {
			this.problems.push(`${this.audio.nonMonotonic} audio timestamp(s) went backwards`);
		}

		// A keyframe every 2 s is what the service forces (GUIDELINE.md §3.2).
		if (videoSec > 4 && this.keyframes > 0) {
			const interval = videoSec / this.keyframes;
			if (interval > 4) {
				this.problems.push(`keyframes are ${interval.toFixed(1)}s apart; expected about 2s`);
			}
		}

		// Both streams share OBS's clock, so their spans should line up.
		const drift = Math.abs(videoSec - audioSec);
		if (this.video.frames > 0 && this.audio.frames > 0 && drift > 0.5) {
			this.problems.push(`video and audio spans differ by ${drift.toFixed(2)}s`);
		}

		return {
			config: { ...this.config, tokenFile: "<path withheld>" },
			video: {
				...this.video,
				keyframes: this.keyframes,
				durationSec: Number(videoSec.toFixed(3)),
				fps: videoSec > 0 ? Number((this.video.frames / videoSec).toFixed(2)) : null,
				kbps: videoSec > 0 ? Number(((this.video.bytes * 8) / videoSec / 1000).toFixed(1)) : null,
			},
			audio: {
				...this.audio,
				durationSec: Number(audioSec.toFixed(3)),
				kbps: audioSec > 0 ? Number(((this.audio.bytes * 8) / audioSec / 1000).toFixed(1)) : null,
			},
			avDriftSec: Number(drift.toFixed(3)),
			checks: { passed: this.problems.length === 0, problems: this.problems },
		};
	}
}
