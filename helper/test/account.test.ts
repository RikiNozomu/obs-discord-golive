/* Tests for the dock's one-shot account commands. Run: node --test test/ */

import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { collect } from "../src/account.ts";

const HELPER = fileURLToPath(new URL("../src/index.ts", import.meta.url));

function runHelper(args: string[]): { code: number | null; lines: string[] } {
	const r = spawnSync(process.execPath, [HELPER, ...args], { encoding: "utf8", input: "" });
	return { code: r.status, lines: r.stderr.trim().split("\n") };
}

function resultOf(lines: string[]): any {
	const line = lines.find((l) => l.startsWith("RESULT "));
	assert.ok(line, `no RESULT line in ${JSON.stringify(lines)}`);
	return JSON.parse(line.slice("RESULT ".length));
}

test("--list --mock reports fake channels and a fake running stream", () => {
	const dir = mkdtempSync(join(tmpdir(), "golive-acct-"));
	let r = runHelper(["--list", "--mock", dir]);
	assert.equal(r.code, 0);
	let res = resultOf(r.lines);
	assert.equal(res.channels.length, 3);
	assert.equal(res.activeStream, null);

	writeFileSync(join(dir, "fake-active-stream"), "");
	res = resultOf(runHelper(["--list", "--mock", dir]).lines);
	assert.equal(res.activeStream.channelName, "Gaming");

	r = runHelper(["--end-stream", "1", "2", "--mock", dir]);
	assert.deepEqual(resultOf(r.lines), { ended: true });
	assert.ok(!existsSync(join(dir, "fake-active-stream")));
});

test("--list without a token file is an error, not a crash", () => {
	const r = runHelper(["--list"]);
	assert.equal(r.code, 1);
	assert.match(r.lines.at(-1) ?? "", /^ERROR --token-file is required/);
});

test("--list with an empty token file says so", () => {
	const dir = mkdtempSync(join(tmpdir(), "golive-acct-"));
	writeFileSync(join(dir, "token"), "\n");
	const r = runHelper(["--list", "--token-file", join(dir, "token")]);
	assert.equal(r.code, 1);
	assert.match(r.lines.at(-1) ?? "", /^ERROR could not connect: the token file is empty/);
});

/** Just enough of the discord.js client shape for collect(). */
function fakeClient() {
	const me = { id: "u1", username: "spare" };
	const perms = (ok: boolean) => ({ has: () => ok });
	const chan = (id: string, name: string, type: string, rawPosition: number, ok = true) => ({
		id,
		name,
		type,
		rawPosition,
		permissionsFor: () => perms(ok),
	});
	const map = <T extends { id: string }>(items: T[]) => new Map(items.map((i) => [i.id, i]));

	const g1chans = [
		chan("c2", "Second", "GUILD_VOICE", 2),
		chan("c1", "First", "GUILD_VOICE", 1),
		chan("t1", "text", "GUILD_TEXT", 0),
		chan("c3", "Locked", "GUILD_VOICE", 3, false),
	];
	const g1 = {
		id: "g1",
		name: "Beta",
		channels: { cache: map(g1chans) },
		voiceStates: { cache: new Map([["u1", { channel: g1chans[0], streaming: true }]]) },
	};
	const g2 = {
		id: "g2",
		name: "Alpha",
		channels: { cache: map([chan("c9", "Only", "GUILD_VOICE", 0)]) },
		voiceStates: { cache: new Map() },
	};
	return { user: me, guilds: { cache: map([g1, g2]) } } as any;
}

test("collect lists streamable voice channels sorted, and finds a running stream", () => {
	const res = collect(fakeClient());
	assert.equal(res.user, "spare");
	assert.deepEqual(
		res.channels.map((c) => `${c.guildName}/${c.channelName}`),
		["Alpha/Only", "Beta/First", "Beta/Second"],
	);
	assert.deepEqual(res.activeStream, {
		guildId: "g1",
		guildName: "Beta",
		channelId: "c2",
		channelName: "Second",
	});
});
