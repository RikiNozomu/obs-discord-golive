#!/usr/bin/env node
/*
 * obs-discord-golive helper.
 *
 * Reads encoded frames from the OBS plugin on stdin (GUIDELINE.md §4) and
 * forwards them to a sink.
 *
 *   --mock <dir>   dump the stream to files and check it (milestone M2)
 *   (default)      stream to Discord (milestone M3, not implemented yet)
 *
 * Run with Node 22.4+ — it executes TypeScript directly, no build step.
 */

import { MockSink } from "./mock.ts";
import { fail, run } from "./ipc.ts";
import type { FrameSink } from "./ipc.ts";

function parseArgs(argv: string[]): { mockDir?: string } {
	const result: { mockDir?: string } = {};

	for (let i = 0; i < argv.length; i++) {
		const arg = argv[i];
		if (arg === "--mock") {
			const dir = argv[++i];
			if (!dir) fail("--mock needs an output directory");
			result.mockDir = dir;
		} else {
			fail(`unknown argument '${arg}'`);
		}
	}
	return result;
}

async function main(): Promise<void> {
	const { mockDir } = parseArgs(process.argv.slice(2));

	let sink: FrameSink;
	if (mockDir) {
		sink = new MockSink(mockDir);
	} else {
		fail("the Discord backend is not implemented yet (milestone M3); run with --mock <dir> for now");
	}

	await run(sink);
}

main().catch((err: unknown) => {
	fail((err as Error).stack ?? String(err));
});
