#!/usr/bin/env node
/*
 * obs-discord-golive helper.
 *
 * Streaming (started by the OBS output): reads encoded frames from the plugin
 * on stdin (GUIDELINE.md §4) and forwards them to a sink.
 *
 *   (default)              stream to Discord Go Live
 *   --mock <dir>           dump the stream to files and check it instead
 *
 * Account commands (started by the dock's Connect button), see account.ts:
 *
 *   --list --token-file <path>
 *   --end-stream <guildId> <channelId> --token-file <path>
 *
 * The token is read from the file; it is never passed on the command line.
 * Run with Node 22.18+ — it executes TypeScript directly, no build step.
 */

import { MockSink } from "./mock.ts";
import { fail, run } from "./ipc.ts";
import type { FrameSink } from "./ipc.ts";

interface Args {
	mockDir?: string;
	tokenFile?: string;
	list?: boolean;
	endStream?: { guildId: string; channelId: string };
}

function parseArgs(argv: string[]): Args {
	const result: Args = {};
	const value = (i: number, what: string): string => {
		const v = argv[i];
		if (!v) fail(`${argv[i - 1]} needs ${what}`);
		return v;
	};

	for (let i = 0; i < argv.length; i++) {
		const arg = argv[i];
		if (arg === "--mock") {
			result.mockDir = value(++i, "an output directory");
		} else if (arg === "--token-file") {
			result.tokenFile = value(++i, "a path");
		} else if (arg === "--list") {
			result.list = true;
		} else if (arg === "--end-stream") {
			const guildId = value(++i, "a server ID and a channel ID");
			const channelId = value(++i, "a server ID and a channel ID");
			result.endStream = { guildId, channelId };
		} else {
			fail(`unknown argument '${arg}'`);
		}
	}
	return result;
}

async function account(args: Args): Promise<void> {
	const acct = await import("./account.ts");
	if (args.mockDir) {
		return args.list ? acct.mockList(args.mockDir) : acct.mockEndStream(args.mockDir);
	}
	if (!args.tokenFile) fail("--token-file is required");
	try {
		if (args.list) await acct.list(args.tokenFile);
		else if (args.endStream) await acct.endStream(args.tokenFile, args.endStream.guildId, args.endStream.channelId);
	} catch (err) {
		fail(`could not connect: ${(err as Error).message}`);
	}
}

async function main(): Promise<void> {
	const args = parseArgs(process.argv.slice(2));

	if (args.list || args.endStream) {
		await account(args);
		process.exit(0); // the Discord client keeps timers alive
	}

	let sink: FrameSink;
	if (args.mockDir) {
		sink = new MockSink(args.mockDir);
	} else {
		// Loaded on demand so mock mode works even without the Discord native modules.
		const { DiscordSink } = await import("./discord.ts");
		sink = new DiscordSink();
	}

	await run(sink);
}

main().catch((err: unknown) => {
	fail((err as Error).stack ?? String(err));
});
