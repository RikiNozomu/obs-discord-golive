/*
 * One-shot account commands the dock runs before streaming:
 *
 *   --list        log in, report the voice channels the account may stream
 *                 to, and whether it is already streaming somewhere
 *   --end-stream  stop the account's Go Live in one channel (e.g. one left
 *                 running on another device)
 *
 * Each prints one "RESULT <json>" line on stderr and exits 0, or "ERROR ..."
 * and exits 1. With --mock, fake data is used and nothing touches Discord.
 */

import { existsSync, rmSync } from "node:fs";
import { join } from "node:path";

import { result } from "./ipc.ts";

export interface VoiceChannelInfo {
	guildId: string;
	guildName: string;
	channelId: string;
	channelName: string;
}

export interface ListResult {
	user: string;
	channels: VoiceChannelInfo[];
	/** The account's Go Live that is running right now, if any. */
	activeStream: VoiceChannelInfo | null;
}

const OP_VOICE_STATE_UPDATE = 4;
const OP_STREAM_DELETE = 19;
const READY_TIMEOUT_MS = 20_000;
const VOICE_TIMEOUT_MS = 5_000;

/* ----------------------------------------------------------------- mock */

/** Test mode: a fake account. Create <mockDir>/fake-active-stream to pretend a stream is running. */
const MOCK_CHANNELS: VoiceChannelInfo[] = [
	{ guildId: "100000000000000001", guildName: "Test server", channelId: "200000000000000001", channelName: "General" },
	{ guildId: "100000000000000001", guildName: "Test server", channelId: "200000000000000002", channelName: "Gaming" },
	{ guildId: "100000000000000002", guildName: "Other server", channelId: "200000000000000003", channelName: "Lounge" },
];

export async function mockList(mockDir: string): Promise<void> {
	const active = existsSync(join(mockDir, "fake-active-stream"));
	await result({ user: "test-user", channels: MOCK_CHANNELS, activeStream: active ? MOCK_CHANNELS[1] : null });
}

export async function mockEndStream(mockDir: string): Promise<void> {
	rmSync(join(mockDir, "fake-active-stream"), { force: true });
	await result({ ended: true });
}

/* ---------------------------------------------------------------- discord */

// Loaded lazily so test mode works without the Discord modules.
type Client = import("@lng2004/discord.js-selfbot-v13").Client;

async function connect(tokenFile: string): Promise<Client> {
	const { Client } = await import("@lng2004/discord.js-selfbot-v13");
	const { describeClose, readToken, withTimeout } = await import("./discord.ts");

	const token = await readToken(tokenFile);
	const client = new Client();

	const ready = new Promise<void>((resolve) => client.once("ready", () => resolve()));
	const closed = new Promise<never>((_, reject) => {
		client.on("shardDisconnect", (event: { code?: number }) => reject(new Error(describeClose(event?.code))));
	});

	await withTimeout(Promise.race([client.login(token).then(() => ready), closed]), READY_TIMEOUT_MS, "login");
	return client;
}

function describe(guild: { id: string; name: string }, channel: { id: string; name: string }): VoiceChannelInfo {
	return { guildId: guild.id, guildName: guild.name, channelId: channel.id, channelName: channel.name };
}

/** Pure: what the dock shows. Exported for tests. */
export function collect(client: Client): ListResult {
	const me = client.user;
	if (!me) throw new Error("logged in, but Discord sent no user");

	const channels: (VoiceChannelInfo & { position: number })[] = [];
	let activeStream: VoiceChannelInfo | null = null;

	for (const guild of client.guilds.cache.values()) {
		const state = guild.voiceStates.cache.get(me.id);
		if (state?.channel && state.streaming) activeStream = describe(guild, state.channel);

		for (const channel of guild.channels.cache.values()) {
			if (channel.type !== "GUILD_VOICE") continue;
			// Unknown permissions (member not cached) are let through; joining will tell.
			const perms = channel.permissionsFor(me);
			if (perms && !perms.has(["VIEW_CHANNEL", "CONNECT", "STREAM"])) continue;
			channels.push({ ...describe(guild, channel), position: channel.rawPosition });
		}
	}

	channels.sort(
		(a, b) => a.guildName.localeCompare(b.guildName) || a.guildId.localeCompare(b.guildId) || a.position - b.position,
	);
	return { user: me.username, channels: channels.map(({ position: _, ...c }) => c), activeStream };
}

export async function list(tokenFile: string): Promise<void> {
	const client = await connect(tokenFile);
	const payload = collect(client);
	client.destroy();
	await result(payload);
}

/**
 * Stops the account's Go Live in guildId/channelId. The stream may belong to
 * another session (phone, browser, a crashed OBS), so after deleting the
 * stream this session briefly joins the channel, which takes the voice
 * connection over from the other session, and then leaves.
 */
export async function endStream(tokenFile: string, guildId: string, channelId: string): Promise<void> {
	const client = await connect(tokenFile);
	const me = client.user;
	if (!me) throw new Error("logged in, but Discord sent no user");

	const send = (op: number, d: unknown) => client.ws.broadcast({ op, d });
	const voiceState = (inChannel: boolean) =>
		new Promise<void>((resolve) => {
			const timer = setTimeout(resolve, VOICE_TIMEOUT_MS);
			client.on("voiceStateUpdate", (_old, state) => {
				if (state.id === me.id && (state.channelId === channelId) === inChannel) {
					clearTimeout(timer);
					resolve();
				}
			});
		});

	send(OP_STREAM_DELETE, { stream_key: `guild:${guildId}:${channelId}:${me.id}` });

	const joined = voiceState(true);
	send(OP_VOICE_STATE_UPDATE, { guild_id: guildId, channel_id: channelId, self_mute: true, self_deaf: true });
	await joined;

	const left = voiceState(false);
	send(OP_VOICE_STATE_UPDATE, { guild_id: guildId, channel_id: null, self_mute: false, self_deaf: false });
	await left;

	const still = client.guilds.cache.get(guildId)?.voiceStates.cache.get(me.id);
	const ended = !(still?.channelId && still.streaming);
	client.destroy();
	await result({ ended });
}
