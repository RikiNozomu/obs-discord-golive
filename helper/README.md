# obs-discord-golive helper

Receives encoded frames from the OBS plugin on stdin and forwards them to Discord.

Keeping the Discord side here, out of the C++ plugin, means a Discord protocol change is a dependency bump rather than a rewrite. See [../GUIDELINE.md](../GUIDELINE.md) §1 and §5.

Runs on **Node 22.4+**, which executes TypeScript directly — no build step, and the mock path has no dependencies at all.

## Run

```bash
node src/index.ts --mock /tmp/dump
```

Reads frames on stdin and writes them to `/tmp/dump` instead of Discord:

| File | What it is |
|---|---|
| `video.h264` | Raw Annex-B H.264, exactly the bytes OBS produced. Play with `ffplay`. |
| `audio.opus` | The Opus packets wrapped in Ogg so players can read them. |
| `report.json` | The config, per-stream counters, and the results of the checks below. |

The sink flags the mistakes that break real streaming: a first frame that is not a keyframe, H.264 that is not Annex-B (an encoder emitting AVCC instead), timestamps going backwards, keyframes further apart than the ~2 s the service forces, and video and audio spans that drift apart.

Without `--mock` the helper streams to Discord — not implemented yet (milestone M3).

## Test

```bash
npm test
```

Protocol tests run anywhere. The end-to-end tests need `ffmpeg` and `ffprobe` and skip without them; they push real ffmpeg-produced H.264 and Opus through the pipe and assert the video comes out byte-identical, that both dumps are still valid media, and that ffmpeg demuxes the audio without complaint.

## Layout

| File | Role |
|---|---|
| `src/protocol.ts` | The wire format, and an incremental parser for frames split across pipe reads. |
| `src/ipc.ts` | Drives a sink from stdin; `READY`/`ERROR`/`LOG`/`STATS` on stderr, with token redaction. |
| `src/mock.ts` | The mock sink: dumps and checks. |
| `src/ogg.ts` | Minimal Ogg Opus muxer, for the audio dump only. |
| `src/index.ts` | Entry point and argument parsing. |

## Tokens

The helper never takes a token on the command line, where other users on the machine could read it. The plugin sends a **path** in the CONFIG frame and the helper reads the file. Every line the helper writes passes through a redactor, so a token cannot reach the OBS log.
