/* Unit tests for the plugin <-> helper wire protocol. Run: node --test test/ */

import test from "node:test";
import assert from "node:assert/strict";

import {
	FLAG_KEYFRAME,
	FrameParser,
	FrameType,
	HEADER_SIZE,
	MAX_PAYLOAD,
	PROTOCOL_VERSION,
	ProtocolError,
	encodeConfig,
	encodeFrame,
	parseConfig,
	type Config,
} from "../src/protocol.ts";
import { redact } from "../src/ipc.ts";
import { _internal } from "../src/ogg.ts";

const CONFIG: Config = {
	v: PROTOCOL_VERSION,
	tokenFile: "/tmp/token.txt",
	guildId: "123456789012345678",
	channelId: "987654321098765432",
	video: { codec: "H264", width: 1920, height: 1080, fps: 60, bitrateKbps: 6000 },
	audio: { codec: "opus", sampleRate: 48000, channels: 2 },
};

test("round-trips a frame", () => {
	const payload = Buffer.from("hello annex-b");
	const [frame] = new FrameParser().push(encodeFrame(FrameType.VIDEO, FLAG_KEYFRAME, 1234567, payload));

	assert.equal(frame.type, FrameType.VIDEO);
	assert.equal(frame.flags, FLAG_KEYFRAME);
	assert.equal(frame.ptsUs, 1234567);
	assert.deepEqual(frame.payload, payload);
});

test("header is exactly 16 bytes", () => {
	assert.equal(encodeFrame(FrameType.STOP, 0, 0).length, HEADER_SIZE);
});

test("reassembles a frame split byte by byte", () => {
	const payload = Buffer.alloc(300, 0xab);
	const wire = encodeFrame(FrameType.VIDEO, 0, 42, payload);
	const parser = new FrameParser();

	const frames = [];
	for (const byte of wire) frames.push(...parser.push(Buffer.from([byte])));

	assert.equal(frames.length, 1);
	assert.deepEqual(frames[0].payload, payload);
	assert.equal(parser.pending, 0);
});

test("parses several frames from one chunk", () => {
	const wire = Buffer.concat([
		encodeFrame(FrameType.VIDEO, FLAG_KEYFRAME, 0, Buffer.alloc(10, 1)),
		encodeFrame(FrameType.AUDIO, 0, 20000, Buffer.alloc(5, 2)),
		encodeFrame(FrameType.STOP, 0, 40000),
	]);
	const frames = new FrameParser().push(wire);

	assert.deepEqual(
		frames.map((f) => f.type),
		[FrameType.VIDEO, FrameType.AUDIO, FrameType.STOP],
	);
	assert.equal(frames[1].ptsUs, 20000);
});

test("holds back a partial frame and reports it as pending", () => {
	const wire = encodeFrame(FrameType.VIDEO, 0, 0, Buffer.alloc(100, 7));
	const parser = new FrameParser();

	assert.equal(parser.push(wire.subarray(0, 60)).length, 0);
	assert.ok(parser.pending > 0);
	assert.equal(parser.push(wire.subarray(60)).length, 1);
	assert.equal(parser.pending, 0);
});

test("handles an empty payload", () => {
	const [frame] = new FrameParser().push(encodeFrame(FrameType.STOP, 0, 999));
	assert.equal(frame.payload.length, 0);
	assert.equal(frame.ptsUs, 999);
});

test("keeps microsecond timestamps exact past 32 bits", () => {
	const pts = 7 * 24 * 3600 * 1_000_000; // a week, in microseconds
	const [frame] = new FrameParser().push(encodeFrame(FrameType.VIDEO, 0, pts));
	assert.equal(frame.ptsUs, pts);
});

test("rejects an unknown frame type", () => {
	const bad = encodeFrame(FrameType.VIDEO, 0, 0);
	bad.writeUInt8(99, 0);
	assert.throws(() => new FrameParser().push(bad), ProtocolError);
});

test("rejects an absurd payload length instead of allocating it", () => {
	const bad = encodeFrame(FrameType.VIDEO, 0, 0);
	bad.writeUInt32LE(MAX_PAYLOAD + 1, 12);
	assert.throws(() => new FrameParser().push(bad), ProtocolError);
});

test("parses a CONFIG frame", () => {
	const [frame] = new FrameParser().push(encodeConfig(CONFIG));
	assert.deepEqual(parseConfig(frame.payload), CONFIG);
});

test("rejects a CONFIG with the wrong protocol version", () => {
	const payload = Buffer.from(JSON.stringify({ ...CONFIG, v: 99 }), "utf8");
	assert.throws(() => parseConfig(payload), ProtocolError);
});

test("rejects a CONFIG that is not JSON", () => {
	assert.throws(() => parseConfig(Buffer.from("not json", "utf8")), ProtocolError);
});

test("redacts tokens from log lines", () => {
	const token = `${"A".repeat(26)}.${"B".repeat(6)}.${"C".repeat(30)}`;
	const out = redact(`login failed for ${token} oh no`);

	assert.ok(!out.includes(token), "the token survived redaction");
	assert.ok(out.includes("<redacted-token>"));
});

test("Ogg CRC matches the reference implementation", () => {
	// Ogg's CRC32 variant: poly 0x04c11db7, init 0, no reflection, no final xor.
	// Cross-checked against ffmpeg's muxer — see ogg-vs-ffmpeg.test.ts, which
	// recomputes the CRC of real pages ffmpeg wrote.
	assert.equal(_internal.crc32(Buffer.from("OggS", "ascii")), 0x5fb0a94f);
	assert.equal(_internal.crc32(Buffer.alloc(0)), 0);
});

test("builds a structurally valid Ogg page", () => {
	const page = _internal.buildPage(0x12345678, 3, 0x02, 960n, Buffer.alloc(400, 9));

	assert.equal(page.subarray(0, 4).toString("ascii"), "OggS");
	assert.equal(page.readUInt8(5), 0x02);
	assert.equal(page.readBigInt64LE(6), 960n);
	assert.equal(page.readUInt32LE(14), 0x12345678);
	assert.equal(page.readUInt32LE(18), 3);

	// 400 bytes lace as 255 + 145.
	assert.equal(page.readUInt8(26), 2);
	assert.equal(page.readUInt8(27), 255);
	assert.equal(page.readUInt8(28), 145);

	// The stored CRC must match a recomputation over the page with it zeroed.
	const stored = page.readUInt32LE(22);
	page.writeUInt32LE(0, 22);
	assert.equal(_internal.crc32(page), stored);
});
