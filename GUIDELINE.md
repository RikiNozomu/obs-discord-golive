# OBS → Discord Go Live plugin: build guideline

**Choices made:** native C++ plugin plus a helper program · helper in Node.js/TypeScript · Windows, macOS and Linux · OBS 31.x

---

## 0. Read this first

| Risk | What it means | Mitigation |
|---|---|---|
| **Terms of Service** | Streaming from a user account ("self-bot") breaks Discord's ToS, and Discord can ban the account. | Use a **spare account**, never your main one. Test in a private server. |
| **Token = full account access** | Anyone who has the token controls the account. | Never commit it, log it or pass it as a command-line argument. See §6. |
| **DAVE end-to-end encryption** | Since about March 2026, Discord rejects voice and Go Live connections from clients that don't support DAVE. | Before writing any other code, confirm the helper library supports DAVE (Milestone 0). |
| **Undocumented protocol** | Go Live isn't a public API, so Discord can break it at any time. | Keep all Discord-specific code in the helper so a fix is just a library upgrade. |

---

## 1. Architecture

```
┌──────────────────────── OBS 31 ────────────────────────┐
│ Scenes → Encoders (H.264, no B-frames · Opus 48 kHz)   │
│                    │ encoder_packet                    │
│   ┌────────────────▼──────────────────┐                │
│   │ obs-discord-golive (C/C++ plugin) │                │
│   │  • service "Discord Go Live"      │                │
│   │  • output  "discord_output"       │                │
│   │  • writer thread + packet queue   │                │
│   └────────────────┬──────────────────┘                │
└────────────────────┼───────────────────────────────────┘
                     │ stdin pipe (framed binary, §4)
         ┌───────────▼─────────────┐
         │ helper (Node/TS)        │   stderr → logged in OBS
         │ discord-video-stream    │
         │ (fork with DAVE)        │
         └───────────┬─────────────┘
                     │ Gateway + voice + RTP/UDP + DAVE
                     ▼
              Discord voice channel (Go Live)
```

**Why split it this way**
- OBS has already encoded the video and audio, so the plugin forwards packets as they are. Nothing is encoded twice.
- The C++ side uses only the stable OBS API and plain pipes. Everything that breaks when Discord changes lives in the helper.

---

## 2. Repository layout

Start from the official template **`obsproject/obs-plugintemplate`**. It ships CMake presets and GitHub Actions for Windows, macOS and Ubuntu, and a `buildspec.json` that pins the OBS version.

```
obs-discord-golive/
├─ buildspec.json              # pin obs-studio 31.x + deps
├─ CMakeLists.txt / CMakePresets.json   (from template)
├─ src/
│  ├─ plugin-main.c            # obs_module_load: register service + output
│  ├─ discord-service.c        # obs_service_info
│  ├─ discord-output.c         # obs_output_info
│  ├─ helper-ipc.c / .h        # spawn helper, framing, writer thread
│  └─ packet-queue.c / .h      # bounded queue + drop policy
├─ data/
│  ├─ locale/en-US.ini
│  └─ helper/                  # packaged helper goes here (found via obs_module_file)
├─ helper/
│  ├─ package.json             # pin library + selfbot client versions exactly
│  ├─ tsconfig.json
│  └─ src/{index.ts, ipc.ts, discord.ts, pacing.ts}
├─ tools/mock-helper/          # dumps packets to .h264/.opus for testing
└─ .github/workflows/          # from template, plus a helper build step
```

---

## 3. The C++ plugin

### 3.1 Register in `obs_module_load`
```c
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-discord-golive", "en-US")

bool obs_module_load(void) {
    obs_register_service(&discord_service_info);
    obs_register_output(&discord_output_info);
    return true;
}
```

### 3.2 Service (`obs_service_info`) and the dock
> **Corrected in M1:** OBS 31's **Settings → Stream** page lists only its built-in services (`rtmp_common`, `rtmp_custom`, `whip_custom`), so a plugin-registered service never appears there. Instead the plugin adds a **"Discord Go Live" dock** (Qt, via `obs_frontend_add_dock_by_id`). The dock stores the settings, creates the service privately (`obs_service_create_private`), creates **its own** x264 and Opus encoders, and starts or stops the output. This also lets you stream to Discord and Twitch at the same time.

| Field | Value |
|---|---|
| `id` | `"discord_golive"` |
| `get_protocol` | `"DISCORD"` (custom; it must match the output's `protocols`) |
| `get_output_type` | `"discord_output"` |
| `get_supported_video_codecs` | `{"h264", NULL}` (add `"hevc"`/`"av1"` later if the library supports them) |
| `get_supported_audio_codecs` | `{"opus", NULL}` |
| `get_properties` | Guild ID, Voice channel ID, **Token file path** (never the token itself). The helper path is not shown: users always run the bundled helper |
| `apply_encoder_settings` | Force `bf=0`, `keyint_sec=2`, `rate_control=CBR`. Audio: 48 kHz stereo, 20 ms frames |

> **Dock flow:** paste the token, press **Connect**. The dock runs the helper once with `--list --token-file <path>`; it logs in and answers with the voice channels the account may stream to (VIEW_CHANNEL + CONNECT + STREAM) and any Go Live the account is already running. The user picks a channel from the list, which enables **Start**. If a stream is already running elsewhere, the button reads **"Stop streaming in …"** and runs `--end-stream <guild> <channel>`: it sends STREAM_DELETE, briefly joins that channel to take the voice session over, leaves, then the dock refreshes the list. **Test mode (developers only, no UI since the public release):** start OBS with `OBS_DISCORD_GOLIVE_MOCK_DIR=<folder>` set. The stream is then dumped into that folder instead of going to Discord, and both commands use fake data; create `<folder>/fake-active-stream` to simulate a running stream. For example: `OBS_DISCORD_GOLIVE_MOCK_DIR=~/golive-test /Applications/OBS.app/Contents/MacOS/OBS`.

> `apply_encoder_settings` matters. **B-frames** break real-time RTP playback, and a long keyframe interval makes viewers who join late wait for a picture.

### 3.3 Output (`obs_output_info`)
```c
struct obs_output_info discord_output_info = {
    .id                   = "discord_output",
    .flags                = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
    .protocols            = "DISCORD",
    .encoded_video_codecs = "h264",
    .encoded_audio_codecs = "opus",
    .get_name = discord_output_name,
    .create   = discord_output_create,
    .destroy  = discord_output_destroy,
    .start    = discord_output_start,
    .stop     = discord_output_stop,
    .encoded_packet = discord_output_packet,
    .get_total_bytes = discord_output_total_bytes,
};
```

**`start`** must return quickly. Do the slow work on a worker thread:
1. `obs_output_can_begin_data_capture` → `obs_output_initialize_encoders`.
2. Read the settings from `obs_output_get_service(output)`.
3. Spawn the helper with `helper-process.c` (posix_spawn / CreateProcessW). **Corrected in M2:** libobs's `os_process_pipe_*` writes through a buffered `FILE*` with no flush (a small CONFIG frame never arrives, so READY never comes) and its destroy waits for the child with no timeout, so it is not used.
4. Send the `CONFIG` frame (§4).
5. Wait for the helper to report `READY` on stderr (`os_process_pipe_read_err`), with a timeout of about 20 s.
6. `obs_output_begin_data_capture(output, 0)`.
7. On failure: `obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED)`.

**`encoded_packet`** runs on the encoder thread, so it **must never block**:
- Copy the packet into the bounded queue (`obs_encoder_packet_ref` or a memcpy).
- The **first video packet, and every keyframe**, gets the SPS/PPS from `obs_encoder_get_extra_data(video_encoder, ...)` placed in front of it.
- Convert timestamps: `pts_us = pts * 1000000 * timebase_num / timebase_den`.
- H.264 packets from OBS encoders are **Annex B** (`00 00 00 01` start codes). Assert this in debug builds.

**Writer thread:** pulls frames from the queue and calls `os_process_pipe_write`.
- **Drop policy** (same idea as OBS's RTMP output): if the queued video exceeds about 700 ms, drop non-keyframe video until the next keyframe. Never drop audio first.
- If the pipe write fails (the helper died), call `obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED)`.

**`stop`:** send a `STOP` frame, flush, give the helper about 3 s to exit, then destroy the pipe. Call `obs_output_end_data_capture`.

### 3.4 Logging
Forward each helper stderr line through `blog(LOG_INFO, "[discord-helper] %s", line)`. Scrub anything that looks like a token before logging.

---

## 4. Pipe protocol (plugin → helper stdin)

All integers are little-endian. One frame:

```
u8   type        1=CONFIG  2=VIDEO  3=AUDIO  4=STOP
u8   flags       bit0 = keyframe
u16  reserved
i64  pts_us
u32  length
u8[] payload     CONFIG: UTF-8 JSON · VIDEO: Annex-B H.264 AU · AUDIO: one Opus packet
```

CONFIG JSON:
```json
{ "v": 1, "tokenFile": "...", "guildId": "...", "channelId": "...",
  "video": { "codec": "H264", "width": 1920, "height": 1080, "fps": 60, "bitrateKbps": 6000 },
  "audio": { "codec": "opus", "sampleRate": 48000, "channels": 2 } }
```

Helper → plugin goes over **stderr, one line per message**: `READY`, `ERROR <msg>`, `LOG <msg>`, `STATS <json>`. The one-shot account commands (`--list`, `--end-stream`, run by the dock, not the output) answer with a single `RESULT <json>` line and exit 0.

Version the protocol (`"v": 1`). If the versions don't match, the helper reports `ERROR` and exits.

---

## 5. The helper (Node.js / TypeScript)

### 5.1 Library choice — **M0 verified 2026-11-07**
Library: **`@dank074/discord-video-stream` v7.0.0** (published 2026-09-13, i.e. after the March 2026 DAVE enforcement). Verified by reading the published source:

| Check | Finding |
|---|---|
| **DAVE support** | **Real, not a stub.** Depends on **`@snazzah/davey` ^0.1.12** (native MLS/DAVE). Implements the full voice opcode set — `MLS_KEY_PACKAGE`, `MLS_EXTERNAL_SENDER`, `MLS_PROPOSALS`, `MLS_COMMIT_WELCOME`, `MLS_WELCOME`, `DAVE_PREPARE_TRANSITION`, `DAVE_EXECUTE_TRANSITION`, `DAVE_PREPARE_EPOCH` — plus epoch transitions and protocol-version negotiation (`max_dave_protocol_version`). |
| **Frame encryption** | `daveSession.encryptOpus(frame)` for audio, `daveSession.encrypt(MediaType.VIDEO, codec, frame)` for video, applied per frame right before the RTP send. |
| **Frame input API** | `sendVideoFrame(frame: Buffer, frametime: number)` and `sendAudioFrame(frame: Buffer, frametime: number)` on the WebRTC wrapper. **These take pre-encoded buffers — no re-encode**, which is exactly our design. |
| **H.264 format** | Annex B with start codes; it splits NALUs and rewrites the SPS VUI itself (`AnnexBHelper`, `SPSVUIRewriter`). **This is the format OBS encoders emit**, so our packets pass through as-is. |
| **Codecs** | H264, H265, VP8, VP9, AV1 (we send H264). |
| **Transport** | WebRTC via `@lng2004/node-datachannel`, not raw UDP/RTP as earlier assumed. |
| **Requirements** | Node **≥22.4.0**; peer dependency **`@lng2004/discord.js-selfbot-v13` ^3.7.2** (a fork, not the original selfbot client). Native deps: `@snazzah/davey`, `node-datachannel`, `node-av`, `sharp`, `zeromq`. |

> Still unproven: that a real account actually connects and streams. That needs a spare Discord account and a live test (§9 step 5). The protocol work above is confirmed present in code; only the live handshake is untested.

**Pin exact versions** in `package.json` (no `^`) once a live test passes.

### 5.2 Flow
```ts
// index.ts (outline; verify the API names against the version you pinned)
const cfg = await ipc.readConfig();                  // first frame
const token = (await fs.readFile(cfg.tokenFile, "utf8")).trim();
const streamer = new Streamer(new Client());
await streamer.client.login(token);
await streamer.joinVoice(cfg.guildId, cfg.channelId);
// Path A (preferred): the library's low-level media connection.
//   Create the Go Live stream, then for each frame from the pipe:
//   sendVideoFrame(au, durationMs) / sendAudioFrame(opus, 20)
// Path B (fallback): remux with no re-encode:
//   pipe frames → `ffmpeg -f h264 -i pipe:… -f ogg -i pipe:… -c copy -f nut -` → playStream(...)
process.stderr.write("READY\n");
```

### 5.3 Pacing (only for Path A)
- Send each frame at wall-clock time `t0 + (pts_us - pts0)`. Sending in bursts makes playback stutter.
- Audio is the master clock. If video falls more than 1 frame behind audio, send it immediately. If it is ahead, wait.

### 5.4 Shutdown and errors
- `STOP` frame, or stdin EOF → leave the stream, leave voice, `client.destroy()`, exit 0.
- Gateway or voice disconnect → `ERROR …` and exit non-zero. The plugin then reports "Disconnected", and OBS's own reconnect settings retry the stream.

### 5.5 Shipping the helper
**Now (until M5):** the build copies `helper/src`, `package.json` and `node_modules` into the plugin (`Contents/Resources/helper/` on macOS, `data/helper/` on Windows and Linux). The plugin runs `helper/src/index.ts` with the user's Node.js (22.18 or newer), so **users never choose a helper path**. Before building, run `npm ci` in `helper/`. CMake warns if `node_modules` is missing. Developers can still run a different helper by setting the `OBS_DISCORD_GOLIVE_HELPER` environment variable to a helper path before starting OBS.

The plugin looks for the helper in this order:

| Order | Where | Who uses it |
|---|---|---|
| 1 | `helper_path` service setting | Automated tests only |
| 2 | `OBS_DISCORD_GOLIVE_HELPER` environment variable | Developers |
| 3 | `helper/discord-helper[.exe]` in the plugin | Users, after M5 |
| 4 | `helper/src/index.ts` in the plugin, run with Node | Users, now |

**Later (M5):**
- Build it as a single executable with **Node SEA** (Node 20+) for each OS and architecture. Users then don't need Node installed.
- Put it in `data/helper/` and locate it with `obs_module_file("helper/discord-helper[.exe]")`.
- macOS: sign the helper with the same identity as the plugin.

---

## 6. Token handling (required)
- The user types the token into the dock (a password field). The dock writes it to `<plugin config>/token`, created owner-only (`0600`), and passes only that **path** in the service settings. The token never goes into `settings.json` or `service.json`, which OBS writes in plain text.
- Restrict the token file: `chmod 600` on macOS/Linux; on Windows, limit its permissions to the current user. Optional upgrade: store it in the OS keychain.
- Pass it to the helper by **file path inside CONFIG**, never on the command line. Other users on the machine can read command lines.
- Redact `[\w-]{24,}\.[\w-]{6}\.[\w-]{27,}` from every log line.

---

## 7. Recommended OBS settings

| Setting | Value |
|---|---|
| Video encoder | x264 / NVENC / AMF / QSV / Apple VT, all **H.264** |
| Profile | baseline or main · **B-frames 0** · keyframe interval 2 s · CBR |
| Resolution / FPS | 1280×720@30 for accounts without Nitro; up to 1920×1080@60 with Nitro (match what the account can do in Discord) |
| Bitrate | 2,500–6,000 kbps |
| Audio | Opus, 48 kHz stereo, 96–128 kbps |

---

## 8. Build per platform (OBS 31)

| OS | Toolchain | Build | Install the plugin to |
|---|---|---|---|
| Windows | VS 2022, CMake ≥ 3.28 | `cmake --preset windows-x64` → `cmake --build --preset windows-x64` | `C:\ProgramData\obs-studio\plugins\obs-discord-golive\` |
| macOS | Xcode 16, CMake ≥ 3.28 | `cmake --preset macos` → `cmake --build --preset macos` | `~/Library/Application Support/obs-studio/plugins/obs-discord-golive.plugin` |
| Linux | GCC 12+/Clang, CMake ≥ 3.28 | `cmake --preset ubuntu-x86_64` → `cmake --build --preset ubuntu-x86_64` | `~/.config/obs-studio/plugins/obs-discord-golive/bin/64bit/` |

Preset names come from the template version you clone. Check `CMakePresets.json`.

CI: keep the template's workflow and add a job that builds the helper with Node SEA for each OS, then copies it into `data/helper/` before packaging.

**Version and build number.** The version lives in `buildspec.json`; the script copies it to the helper's `package.json` and `package-lock.json` so they always match. The build number goes up by one on every CMake configure (`cmake --preset macos`), not on every build. It is kept in `cmake/.CMakeBuildNumber`, which is not committed. Run each command from the project root, then configure and build again.

Example, starting from version `0.1.0` with build 8 as the last build:

| Command | Result |
|---|---|
| `node scripts/version.mjs` | `version 0.1.0` · `last build 8` · `next build 9` (nothing changes) |
| `cmake --preset macos` | Configures with build **9**; the next configure uses 10 |
| `node scripts/version.mjs patch` | `version 0.1.0 -> 0.1.1, build number restarts at 1` |
| `node scripts/version.mjs minor` | `version 0.1.1 -> 0.2.0, build number restarts at 1` |
| `node scripts/version.mjs major` | `version 0.2.0 -> 1.0.0, build number restarts at 1` |
| `node scripts/version.mjs set 1.2.3` | `version 1.0.0 -> 1.2.3, build number restarts at 1` |
| `node scripts/version.mjs build 42` | `next build number: 42` (the version stays the same) |
| `node scripts/version.mjs set 1.2` | `error: usage: set <major.minor.patch>, e.g. set 1.2.3` (nothing changes) |
| `node scripts/version.mjs build x` | `error: usage: build <number>, a whole number of 1 or more` (nothing changes) |

After rebuilding, the running plugin shows the numbers in two places:
- The OBS log (Help → Log Files): `[obs-discord-golive] plugin loaded successfully (version 1.2.3, build 42)`.
- The bottom of the dock: `Version 1.2.3 (build 42)`.

---

## 9. Testing plan

1. **Plugin loads:** the "Discord Go Live" dock appears under Docks, and the settings save (automated by `tests/run-module-test.sh`).
2. **Mock helper:** `tools/mock-helper` writes VIDEO frames to `out.h264` and AUDIO frames to an Ogg/Opus file. Check them with `ffprobe` and `ffplay`, and confirm the timestamps increase steadily.
3. **Stress test:** throttle the mock helper. Confirm OBS stays responsive, frames get dropped, and the dropped-frames counter goes up.
4. **Helper crash:** kill the helper mid-stream. OBS should show "Disconnected" and try to reconnect.
5. **Real Discord test** (spare account, private server): join, start Go Live, watch from a second account, and check sync over 30+ minutes.
6. **Run on all 3 operating systems** in CI, plus a manual smoke test on each.

---

## 10. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 🟡 | DAVE check | **Code audit done** (§5.1): DAVE is really implemented and the frame API takes pre-encoded Annex-B H.264 / Opus. **Live test still owed** — needs a spare Discord account |
| M1 ✅ | Plugin skeleton from the template | Builds clean (GCC and Clang, warnings as errors) against OBS 31.1.1; dock visible in OBS; headless module test passes |
| M2 ✅ | Output + pipe + mock helper | Plugin spawns the helper, sends CONFIG, waits for READY, streams through a bounded queue with a writer thread, drops video when the helper lags >700 ms. Verified headless on macOS against OBS 32.2 with real x264/Opus: mock dump decodes cleanly (H.264 Main, no B-frames, 2 s keyframes, A/V drift 13 ms); helper crash → Disconnected; missing/failing/hanging helper → clear errors, no orphan process. Windows half of `helper-process.c` written but not yet compiled |
| M3 🟡 | Real helper (Path A or B) | **Code done (Path A)**: `helper/src/discord.ts` logs in, joins voice, starts Go Live and sends OBS frames via `sendVideoFrame`/`sendAudioFrame`, paced by `pacing.ts` (80 ms jitter buffer, shared A/V clock). Verified against the real gateway up to login (a bad token gives a clear error in OBS). **Live test still owed**: needs a spare account (§9 step 5) |
| M4 | Robustness | Drop policy, reconnect, token redaction, stats |
| M5 | Packaging | Installers or zips with the bundled helper for each OS |

---

## 11. Checklist before each release
- [ ] Helper library and selfbot client pinned to exact versions; DAVE confirmed working
- [ ] No token in logs, `argv`, crash dumps or committed files
- [ ] B-frames forced off; 2 s keyframe interval
- [ ] `encoded_packet` never blocks (checked with a stalled helper)
- [ ] Builds and smoke tests pass on Windows, macOS and Linux
- [ ] README says plainly that this breaks Discord's ToS and accounts may be banned
