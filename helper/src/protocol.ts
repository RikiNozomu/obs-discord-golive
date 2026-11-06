/*
 * Wire protocol between the OBS plugin and this helper.
 *
 * The plugin writes length-prefixed binary frames to the helper's stdin.
 * See GUIDELINE.md §4. All integers are little-endian.
 *
 *   u8   type        1=CONFIG 2=VIDEO 3=AUDIO 4=STOP
 *   u8   flags       bit0 = keyframe
 *   u16  reserved
 *   i64  pts_us      presentation timestamp in microseconds
 *   u32  length      payload byte count
 *   u8[] payload     CONFIG: UTF-8 JSON | VIDEO: Annex-B H.264 AU | AUDIO: one Opus packet
 */

export const PROTOCOL_VERSION = 1;
export const HEADER_SIZE = 16;

/** Guard against a corrupt stream claiming an absurd payload size. */
export const MAX_PAYLOAD = 64 * 1024 * 1024;

export const FrameType = {
	CONFIG: 1,
	VIDEO: 2,
	AUDIO: 3,
	STOP: 4,
} as const;

export type FrameTypeValue = (typeof FrameType)[keyof typeof FrameType];

export const FLAG_KEYFRAME = 0x01;

export interface Frame {
	type: FrameTypeValue;
	flags: number;
	ptsUs: number;
	payload: Buffer;
}

export interface VideoConfig {
	codec: string;
	width: number;
	height: number;
	fps: number;
	bitrateKbps: number;
}

export interface AudioConfig {
	codec: string;
	sampleRate: number;
	channels: number;
}

export interface Config {
	v: number;
	tokenFile: string;
	guildId: string;
	channelId: string;
	video: VideoConfig;
	audio: AudioConfig;
}

const VALID_TYPES = new Set<number>(Object.values(FrameType));

export function isKeyframe(frame: Frame): boolean {
	return (frame.flags & FLAG_KEYFRAME) !== 0;
}

/** Build one frame. Used by the tests and by any tool that feeds a helper. */
export function encodeFrame(
	type: FrameTypeValue,
	flags: number,
	ptsUs: number,
	payload: Buffer = Buffer.alloc(0),
): Buffer {
	const header = Buffer.alloc(HEADER_SIZE);
	header.writeUInt8(type, 0);
	header.writeUInt8(flags & 0xff, 1);
	header.writeUInt16LE(0, 2);
	header.writeBigInt64LE(BigInt(Math.trunc(ptsUs)), 4);
	header.writeUInt32LE(payload.length, 12);
	return Buffer.concat([header, payload]);
}

export function encodeConfig(config: Config): Buffer {
	return encodeFrame(FrameType.CONFIG, 0, 0, Buffer.from(JSON.stringify(config), "utf8"));
}

export class ProtocolError extends Error {}

/**
 * Incremental parser. Chunks arriving on a pipe split anywhere, so feed every
 * chunk to push() and act on the frames it returns.
 */
export class FrameParser {
	private buf: Buffer = Buffer.alloc(0);

	push(chunk: Buffer): Frame[] {
		this.buf = this.buf.length === 0 ? chunk : Buffer.concat([this.buf, chunk]);

		const frames: Frame[] = [];
		let offset = 0;

		while (this.buf.length - offset >= HEADER_SIZE) {
			const type = this.buf.readUInt8(offset);
			const flags = this.buf.readUInt8(offset + 1);
			const ptsUs = Number(this.buf.readBigInt64LE(offset + 4));
			const length = this.buf.readUInt32LE(offset + 12);

			if (!VALID_TYPES.has(type)) {
				throw new ProtocolError(`unknown frame type ${type} at offset ${offset}`);
			}
			if (length > MAX_PAYLOAD) {
				throw new ProtocolError(`payload of ${length} bytes exceeds the ${MAX_PAYLOAD}-byte limit`);
			}

			// Wait for the rest of this frame.
			if (this.buf.length - offset - HEADER_SIZE < length) break;

			const start = offset + HEADER_SIZE;
			frames.push({
				type: type as FrameTypeValue,
				flags,
				ptsUs,
				payload: this.buf.subarray(start, start + length),
			});
			offset = start + length;
		}

		this.buf = offset === 0 ? this.buf : this.buf.subarray(offset);
		return frames;
	}

	/** Bytes held back waiting for the rest of a frame. Non-zero at EOF means a truncated stream. */
	get pending(): number {
		return this.buf.length;
	}
}

export function parseConfig(payload: Buffer): Config {
	let parsed: unknown;
	try {
		parsed = JSON.parse(payload.toString("utf8"));
	} catch (err) {
		throw new ProtocolError(`CONFIG is not valid JSON: ${(err as Error).message}`);
	}

	const config = parsed as Config;
	if (config?.v !== PROTOCOL_VERSION) {
		throw new ProtocolError(`unsupported protocol version ${config?.v}, expected ${PROTOCOL_VERSION}`);
	}
	return config;
}
