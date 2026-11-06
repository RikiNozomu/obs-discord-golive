# Discord Go Live for OBS

An OBS Studio 31 plugin that streams to a Discord voice channel (Go Live).
It adds a **Discord Go Live** dock with its own settings and Start/Stop button. The dock runs alongside OBS's normal stream and recording.

> **Warning:** streaming from a user account ("self-bot") breaks Discord's Terms of Service, and Discord may ban the account. Use a spare account.

See [GUIDELINE.md](GUIDELINE.md) for the full design.

## Status

| Milestone | State |
|---|---|
| M0: confirm a helper library with DAVE support | not started (needs a Discord account) |
| **M1: plugin skeleton, service, output, dock** | **done** |
| M2: helper process, pipe protocol, mock helper | next |
| M3–M5 | not started |

Clicking **Start Discord stream** currently checks the settings, creates the x264 and Opus encoders, and then stops with the message "The streaming pipeline is not built yet (milestone M2)".

## Build

The project uses the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) build system, pinned to OBS 31.1.1 in `buildspec.json`.

**Windows / macOS:** the presets download the OBS SDK and Qt automatically.

```bash
cmake --preset windows-x64      # or: macos
```

```bash
cmake --build --preset windows-x64
```

**Linux:** install OBS and Qt 6 development files first (the CI uses `ppa:obsproject/obs-studio` and `qt6-base-dev`), then:

```bash
cmake --preset ubuntu-x86_64
```

```bash
cmake --build --preset ubuntu-x86_64
```

If libobs is somewhere other than a system path, add `-DCMAKE_PREFIX_PATH=/path/to/obs-sdk` to the first command.

## Test

```bash
tests/run-module-test.sh /path/to/obs-sdk
```

This loads the built plugin into a headless libobs and checks that the service and output register, that the settings are validated, that the encoder settings are forced (no B-frames, 2 s keyframes, CBR), and that start failures give clear messages.

## Settings storage

The dock saves its settings to `obs-discord-golive/settings.json` in OBS's plugin config folder. Only the **path** to the token file is stored, never the token itself.
