#!/usr/bin/env node
/*
 * Show or change the plugin's version and build number.
 *
 *   node scripts/version.mjs                 show version and build number
 *   node scripts/version.mjs patch           0.1.0 -> 0.1.1   (also: minor, major)
 *   node scripts/version.mjs set 1.2.3       set an exact version
 *   node scripts/version.mjs build 42        make the next build number 42
 *
 * The version lives in buildspec.json (CMake reads it from there) and is
 * copied to helper/package.json and helper/package-lock.json so the plugin
 * and helper always agree. Changing the version restarts the build number
 * at 1.
 *
 * The build number lives in cmake/.CMakeBuildNumber (not committed). It holds
 * the last number used; every CMake configure (cmake --preset macos) adds 1.
 * Run configure again after this script so the build picks up the change.
 */

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const BUILDSPEC = join(ROOT, "buildspec.json");
const BUILD_NUMBER = join(ROOT, "cmake", ".CMakeBuildNumber");
const HELPER_FILES = ["helper/package.json", "helper/package-lock.json"].map((f) => join(ROOT, f));

const SEMVER = /^(\d+)\.(\d+)\.(\d+)$/;

function die(message) {
	console.error(`error: ${message}`);
	process.exit(1);
}

function readJson(path) {
	return JSON.parse(readFileSync(path, "utf8"));
}

/* All of these files use 4-space indentation and a trailing newline. */
function writeJson(path, data) {
	writeFileSync(path, `${JSON.stringify(data, null, 4)}\n`);
}

function currentVersion() {
	return readJson(BUILDSPEC).version;
}

/** Last build number used, or 0 if none yet. */
function lastBuild() {
	if (!existsSync(BUILD_NUMBER)) return 0;
	const n = Number.parseInt(readFileSync(BUILD_NUMBER, "utf8").trim(), 10);
	return Number.isInteger(n) && n >= 0 ? n : 0;
}

function setNextBuild(next) {
	writeFileSync(BUILD_NUMBER, String(next - 1));
}

function setVersion(version) {
	const spec = readJson(BUILDSPEC);
	spec.version = version;
	writeJson(BUILDSPEC, spec);

	for (const path of HELPER_FILES) {
		if (!existsSync(path)) continue;
		const pkg = readJson(path);
		pkg.version = version;
		if (pkg.packages?.[""]) pkg.packages[""].version = version;
		writeJson(path, pkg);
	}
}

function bump(version, part) {
	const m = SEMVER.exec(version);
	if (!m) die(`buildspec.json has version '${version}', expected x.y.z`);
	let [major, minor, patch] = m.slice(1).map(Number);
	if (part === "major") [major, minor, patch] = [major + 1, 0, 0];
	else if (part === "minor") [minor, patch] = [minor + 1, 0];
	else patch += 1;
	return `${major}.${minor}.${patch}`;
}

function show() {
	const last = lastBuild();
	console.log(`version       ${currentVersion()}`);
	console.log(`last build    ${last === 0 ? "(none yet)" : last}`);
	console.log(`next build    ${last + 1}  (used by the next 'cmake --preset ...')`);
}

const [command, arg] = process.argv.slice(2);

switch (command) {
	case undefined:
	case "show":
		show();
		break;

	case "major":
	case "minor":
	case "patch": {
		const from = currentVersion();
		const to = bump(from, command);
		setVersion(to);
		setNextBuild(1);
		console.log(`version ${from} -> ${to}, build number restarts at 1`);
		break;
	}

	case "set": {
		if (!arg || !SEMVER.test(arg)) die("usage: set <major.minor.patch>, e.g. set 1.2.3");
		const from = currentVersion();
		setVersion(arg);
		if (arg !== from) setNextBuild(1);
		console.log(`version ${from} -> ${arg}${arg !== from ? ", build number restarts at 1" : ""}`);
		break;
	}

	case "build": {
		const n = Number(arg);
		if (!Number.isInteger(n) || n < 1) die("usage: build <number>, a whole number of 1 or more");
		setNextBuild(n);
		console.log(`next build number: ${n}`);
		break;
	}

	default:
		die(`unknown command '${command}'. Use: show, patch, minor, major, set <x.y.z>, build <n>`);
}

if (command && command !== "show") {
	console.log("Now run: cmake --preset macos && cmake --build --preset macos");
}
