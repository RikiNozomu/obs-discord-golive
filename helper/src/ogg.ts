/*
 * Minimal Ogg Opus muxer, just enough to make the mock helper's audio dump
 * playable by ffplay/ffprobe. Not used on the Discord path, where Opus
 * packets go out over RTP unwrapped.
 *
 * One packet per page: valid Ogg, slightly wasteful, and much easier to get
 * right than packing pages. This only ever writes debug artifacts.
 */

import type { WriteStream } from "node:fs";

/** Ogg's CRC32: poly 0x04c11db7, no reflection, init 0, no final xor. */
const CRC_TABLE = (() => {
	const table = new Uint32Array(256);
	for (let i = 0; i < 256; i++) {
		let r = i << 24;
		for (let j = 0; j < 8; j++) {
			r = (r & 0x80000000) !== 0 ? ((r << 1) ^ 0x04c11db7) >>> 0 : (r << 1) >>> 0;
		}
		table[i] = r >>> 0;
	}
	return table;
})();

function crc32(buf: Buffer): number {
	let crc = 0;
	for (const byte of buf) {
		crc = ((crc << 8) ^ CRC_TABLE[((crc >>> 24) ^ byte) & 0xff]) >>> 0;
	}
	return crc >>> 0;
}

const HEADER_TYPE_CONTINUED = 0x01;
const HEADER_TYPE_BOS = 0x02;
const HEADER_TYPE_EOS = 0x04;

/** Opus granule positions are always in 48 kHz samples. */
const GRANULE_RATE = 48000;

function buildPage(
	serial: number,
	sequence: number,
	headerType: number,
	granule: bigint,
	packet: Buffer,
): Buffer {
	// Lacing: 255-byte segments, a final short one terminates the packet.
	const segments: number[] = [];
	let remaining = packet.length;
	while (remaining >= 255) {
		segments.push(255);
		remaining -= 255;
	}
	segments.push(remaining);

	if (segments.length > 255) {
		throw new Error(`packet of ${packet.length} bytes needs more than one Ogg page`);
	}

	const header = Buffer.alloc(27 + segments.length);
	header.write("OggS", 0, "ascii");
	header.writeUInt8(0, 4); // stream structure version
	header.writeUInt8(headerType, 5);
	header.writeBigInt64LE(granule, 6);
	header.writeUInt32LE(serial, 14);
	header.writeUInt32LE(sequence, 18);
	header.writeUInt32LE(0, 22); // CRC placeholder
	header.writeUInt8(segments.length, 26);
	for (const [i, seg] of segments.entries()) header.writeUInt8(seg, 27 + i);

	const page = Buffer.concat([header, packet]);
	page.writeUInt32LE(crc32(page), 22);
	return page;
}

function opusHead(channels: number, inputSampleRate: number): Buffer {
	const buf = Buffer.alloc(19);
	buf.write("OpusHead", 0, "ascii");
	buf.writeUInt8(1, 8); // version
	buf.writeUInt8(channels, 9);
	buf.writeUInt16LE(0, 10); // pre-skip: 0, so nothing is trimmed from the dump
	buf.writeUInt32LE(inputSampleRate, 12);
	buf.writeInt16LE(0, 16); // output gain
	buf.writeUInt8(0, 18); // channel mapping family
	return buf;
}

function opusTags(): Buffer {
	const vendor = Buffer.from("obs-discord-golive mock helper", "utf8");
	const buf = Buffer.alloc(8 + 4 + vendor.length + 4);
	buf.write("OpusTags", 0, "ascii");
	buf.writeUInt32LE(vendor.length, 8);
	vendor.copy(buf, 12);
	buf.writeUInt32LE(0, 12 + vendor.length); // no user comments
	return buf;
}

export class OggOpusWriter {
	private sequence = 0;
	private granule = 0n;
	private readonly serial: number;
	private readonly out: WriteStream;
	private pending?: { packet: Buffer; granule: bigint };

	constructor(out: WriteStream, channels: number, sampleRate: number) {
		this.out = out;
		this.serial = (Math.random() * 0xffffffff) >>> 0;
		this.out.write(buildPage(this.serial, this.sequence++, HEADER_TYPE_BOS, 0n, opusHead(channels, sampleRate)));
		this.out.write(buildPage(this.serial, this.sequence++, 0, 0n, opusTags()));
	}

	/**
	 * Append one Opus packet. `durationMs` advances the granule clock; OBS is
	 * configured for 20 ms frames (GUIDELINE.md §3.2).
	 *
	 * Each packet is held back until the next one arrives, so end() can mark
	 * the final data page end-of-stream. Emitting a separate empty page
	 * instead would put a zero-length packet on the stream, which decoders
	 * reject as invalid Opus.
	 */
	write(packet: Buffer, durationMs = 20): void {
		this.granule += BigInt(Math.round((durationMs * GRANULE_RATE) / 1000));
		this.flushPending(0);
		this.pending = { packet, granule: this.granule };
	}

	/** Mark the stream finished. Does not close the underlying stream. */
	end(): void {
		if (this.pending) {
			this.flushPending(HEADER_TYPE_EOS);
			return;
		}
		// No audio at all: an empty page is the only way to terminate.
		this.out.write(buildPage(this.serial, this.sequence++, HEADER_TYPE_EOS, this.granule, Buffer.alloc(0)));
	}

	private flushPending(headerType: number): void {
		if (!this.pending) return;
		this.out.write(buildPage(this.serial, this.sequence++, headerType, this.pending.granule, this.pending.packet));
		this.pending = undefined;
	}
}

export const _internal = { crc32, buildPage, HEADER_TYPE_CONTINUED };
