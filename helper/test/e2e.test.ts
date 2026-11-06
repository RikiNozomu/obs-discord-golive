/*
 * End-to-end tests for the helper, using real media produced by ffmpeg.
 *
 * These prove the property milestone M2 is about: whatever the plugin writes
 * into the pipe comes out the far side byte for byte, and the dumps are still
 * valid, playable streams.
 *
 * Skipped automatically when ffmpeg/ffprobe are not installed.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { FLAG_KEYFRAME, FrameType, PROTOCOL_VERSION, encodeConfig, encodeFrame, type Config } from "../src/protocol.ts";

const HELPER = fileURLToPath(new URL("../src/index.ts", import.meta.url));

function has(cmd: string): boolean {
	return spawnSync(cmd, ["-version"], { stdio: "ignore" }).status === 0;
}

const ffmpegMissing = !has("ffmpeg") || !has("ffprobe");
const options = { skip: ffmpegMissing ? "ffmpeg/ffprobe not installed" : false };

function work(): string {
	return mkdtempSync(join(tmpdir(), "obs-discord-golive-e2e-"));
}

function ffprobe(args: string[]): string {
	return execFileSync("ffprobe", ["-v", "error", ...args], { encoding: "utf8" }).trim();
}

/** Split an Ogg stream back into its packets, so we can re-mux real Opus. */
function demuxOgg(buf: Buffer): Buffer[] {
	const packets: Buffer[] = [];
	let pending: Buffer[] = [];
	let offset = 0;

	while (offset + 27 <= buf.length && buf.subarray(offset, offset + 4).toString("ascii") === "OggS") {
		const segmentCount = buf.readUInt8(offset + 26);
		const lacing = buf.subarray(offset + 27, offset + 27 + segmentCount);
		let body = offset + 27 + segmentCount;

		for (const size of lacing) {
			pending.push(buf.subarray(body, body + size));
			body += size;
			if (size < 255) {
				packets.push(Buffer.concat(pending));
				pending = [];
			}
		}
		offset = body;
	}
	return packets;
}

/** Feed frames to the helper in mock mode and return the parsed report. */
function runHelper(outDir: string, wire: Buffer) {
	const result = spawnSync(process.execPath, [HELPER, "--mock", outDir], {
		input: wire,
		encoding: "buffer",
		maxBuffer: 256 * 1024 * 1024,
	});

	const stderr = result.stderr.toString("utf8");
	assert.equal(result.status, 0, `helper exited ${result.status}\n${stderr}`);
	assert.ok(stderr.includes("READY"), `helper never reported READY\n${stderr}`);

	return {
		report: JSON.parse(readFileSync(join(outDir, "report.json"), "utf8")),
		stderr,
	};
}

function config(overrides: Partial<Config> = {}): Config {
	return {
		v: PROTOCOL_VERSION,
		tokenFile: "/dev/null",
		guildId: "123456789012345678",
		channelId: "987654321098765432",
		video: { codec: "H264", width: 320, height: 240, fps: 30, bitrateKbps: 1000 },
		audio: { codec: "opus", sampleRate: 48000, channels: 2 },
		...overrides,
	};
}

test("carries real H.264 and Opus through the pipe without corrupting them", options, () => {
	const dir = work();
	const srcH264 = join(dir, "src.h264");
	const srcOpus = join(dir, "src.opus");

	// 2 seconds at 30 fps, keyframe every 60 frames, plus a matching tone.
	execFileSync("ffmpeg", [
		"-v", "error",
		"-f", "lavfi", "-i", "testsrc=size=320x240:rate=30:duration=2",
		"-c:v", "libx264", "-g", "60", "-bf", "0", "-pix_fmt", "yuv420p",
		"-bsf:v", "h264_mp4toannexb", "-f", "h264", "-y", srcH264,
	]);
	execFileSync("ffmpeg", [
		"-v", "error",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=2",
		"-ac", "2", "-c:a", "libopus", "-b:a", "96k", "-y", srcOpus,
	]);

	const videoBytes = readFileSync(srcH264);
	const opusPackets = demuxOgg(readFileSync(srcOpus));
	assert.ok(opusPackets.length > 50, `expected real Opus packets, got ${opusPackets.length}`);

	// Chunk the video into 60 pieces standing in for access units. The stream
	// opens with SPS/PPS/IDR, so marking the first chunk a keyframe is accurate.
	const pieces: Buffer[] = [];
	const size = Math.ceil(videoBytes.length / 60);
	for (let i = 0; i < videoBytes.length; i += size) pieces.push(videoBytes.subarray(i, i + size));

	const frames: Buffer[] = [encodeConfig(config())];
	pieces.forEach((piece, i) => {
		frames.push(encodeFrame(FrameType.VIDEO, i === 0 ? FLAG_KEYFRAME : 0, Math.round((i * 1e6) / 30), piece));
	});
	opusPackets.forEach((packet, i) => {
		frames.push(encodeFrame(FrameType.AUDIO, 0, i * 20_000, packet));
	});
	frames.push(encodeFrame(FrameType.STOP, 0, 2_000_000));

	const { report } = runHelper(dir, Buffer.concat(frames));

	// 1. Nothing was lost or reordered.
	assert.equal(report.video.frames, pieces.length);
	assert.equal(report.audio.frames, opusPackets.length);
	assert.equal(report.video.nonMonotonic, 0);
	assert.equal(report.audio.nonMonotonic, 0);

	// 2. The video dump is byte-identical to what ffmpeg produced.
	assert.deepEqual(readFileSync(join(dir, "video.h264")), videoBytes, "video bytes changed in transit");

	// 3. The dumps are still valid media.
	assert.equal(ffprobe(["-select_streams", "v:0", "-show_entries", "stream=codec_name",
		"-of", "default=nw=1:nk=1", join(dir, "video.h264")]), "h264");

	const audioInfo = ffprobe(["-select_streams", "a:0", "-show_entries",
		"stream=codec_name,sample_rate,channels", "-of", "default=nw=1:nk=1", join(dir, "audio.opus")]);
	assert.deepEqual(audioInfo.split("\n"), ["opus", "48000", "2"]);

	// 4. Our Ogg muxing is sound: the audio decodes to about 2 seconds, and
	//    ffmpeg reports no demux errors while doing it. The silent-warning
	//    case matters — an empty end-of-stream page once produced a stream
	//    that still decoded but logged "Invalid data found".
	const decode = spawnSync(
		"ffmpeg",
		["-v", "error", "-i", join(dir, "audio.opus"), "-f", "s16le", "-ac", "2", "-ar", "48000", "-"],
		{ maxBuffer: 64 * 1024 * 1024 },
	);
	assert.equal(decode.status, 0, decode.stderr.toString("utf8"));
	assert.equal(decode.stderr.toString("utf8").trim(), "", "ffmpeg complained while demuxing our Ogg");

	const seconds = decode.stdout.length / (48000 * 2 * 2);
	assert.ok(Math.abs(seconds - 2) < 0.2, `decoded audio was ${seconds.toFixed(2)}s, expected about 2s`);

	// 5. The sink found nothing wrong.
	assert.deepEqual(report.checks.problems, []);
	assert.equal(report.checks.passed, true);
});

test("flags a first video frame that is not a keyframe", options, () => {
	const dir = work();
	const { report } = runHelper(
		dir,
		Buffer.concat([
			encodeConfig(config()),
			// No keyframe flag, and no Annex-B start code.
			encodeFrame(FrameType.VIDEO, 0, 0, Buffer.from([0x41, 0x9a, 0x00])),
			encodeFrame(FrameType.AUDIO, 0, 0, Buffer.alloc(8, 1)),
			encodeFrame(FrameType.STOP, 0, 1000),
		]),
	);

	assert.equal(report.checks.passed, false);
	assert.ok(report.checks.problems.some((p: string) => p.includes("not a keyframe")));
	assert.ok(report.checks.problems.some((p: string) => p.includes("Annex-B")));
});

test("flags timestamps that go backwards", options, () => {
	const dir = work();
	const { report } = runHelper(
		dir,
		Buffer.concat([
			encodeConfig(config()),
			encodeFrame(FrameType.VIDEO, FLAG_KEYFRAME, 1_000_000, Buffer.from([0, 0, 0, 1, 0x65])),
			encodeFrame(FrameType.VIDEO, 0, 500_000, Buffer.from([0, 0, 0, 1, 0x41])),
			encodeFrame(FrameType.AUDIO, 0, 0, Buffer.alloc(8, 1)),
			encodeFrame(FrameType.STOP, 0, 2_000_000),
		]),
	);

	assert.equal(report.video.nonMonotonic, 1);
	assert.ok(report.checks.problems.some((p: string) => p.includes("backwards")));
});

test("rejects a stream that does not start with CONFIG", options, () => {
	const dir = work();
	const result = spawnSync(process.execPath, [HELPER, "--mock", dir], {
		input: encodeFrame(FrameType.VIDEO, FLAG_KEYFRAME, 0, Buffer.alloc(4)),
		encoding: "utf8",
	});

	assert.notEqual(result.status, 0);
	assert.ok(result.stderr.includes("ERROR"), result.stderr);
	assert.ok(result.stderr.includes("CONFIG"), result.stderr);
});

test("reports a truncated stream instead of exiting quietly", options, () => {
	const dir = work();
	const good = Buffer.concat([encodeConfig(config()), encodeFrame(FrameType.VIDEO, FLAG_KEYFRAME, 0, Buffer.alloc(500, 3))]);
	const result = spawnSync(process.execPath, [HELPER, "--mock", dir], {
		input: good.subarray(0, good.length - 100), // cut mid-payload
		encoding: "utf8",
	});

	assert.notEqual(result.status, 0);
	assert.ok(result.stderr.includes("incomplete frame"), result.stderr);
});
