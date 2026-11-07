# Discord Go Live for OBS

Stream straight from OBS Studio into a Discord voice channel as **Go Live** (screen share), without a second app.

The plugin adds a **Discord Go Live** panel to OBS. Log in, pick a voice channel, press Start. It runs alongside your normal OBS stream and recording, so you can stream to Twitch or YouTube and Discord at the same time.

> [!WARNING]
> This plugin streams from a **Discord user account** (a "self-bot"). That breaks [Discord's Terms of Service](https://discord.com/terms), and Discord may ban the account. **Use a spare account, never your main one.** You use this plugin at your own risk.

![The Discord Go Live dock in OBS](docs/images/dock.png)

## Features

- **Go Live from OBS:** viewers in the voice channel see your OBS scene and hear your OBS audio.
- **Pick from a list:** after you connect, the dock lists the voice channels the account can stream in. No IDs to copy.
- **Stream quality presets:** 720p 30 fps (works without Nitro), 1080p 30 fps, 1080p 60 fps, or the same as OBS. The bitrate is chosen for you. Your OBS settings and other outputs are not changed.
- **Stops a stream left running elsewhere:** if the account is already streaming on another device, the dock offers **Stop streaming in …** before you start here.
- **Runs next to your normal stream:** the Discord stream uses its own encoders.
- **Your token stays on your computer:** it is saved in a file only your user account can read, and only sent to Discord. It never appears in OBS logs.
- **Reconnects** by itself if the connection to Discord drops.

## Requirements

| | |
|---|---|
| OBS Studio | 31 or newer |
| macOS | 12 Monterey or newer, Apple Silicon or Intel |
| Windows | 10 or 11, 64-bit |
| Node.js | **22.18 or newer**, from [nodejs.org](https://nodejs.org) (the plugin uses it in the background) |
| Discord | a spare account that can join and stream in the voice channel |

## Install

Close OBS before installing.

### macOS

1. Install [Node.js](https://nodejs.org) (the LTS version is fine) if you don't have it.
2. Download `obs-discord-golive-<version>-macos-universal.pkg` from the [Releases](../../releases) page and open it.
3. If macOS says the package "cannot be opened because it is from an unidentified developer": open **System Settings → Privacy & Security**, scroll down, and click **Open Anyway**.
4. Start OBS. Open the panel from **Docks → Discord Go Live**.

<details>
<summary>Manual install with the .zip instead</summary>

Unzip `obs-discord-golive-<version>-macos-universal.zip` and move `obs-discord-golive.plugin` to
`~/Library/Application Support/obs-studio/plugins/` (create the `plugins` folder if it does not exist).
</details>

### Windows

1. Install [Node.js](https://nodejs.org) (the LTS version is fine) if you don't have it.
2. Download `obs-discord-golive-<version>-windows-x64.zip` from the [Releases](../../releases) page.
3. Unzip it into `C:\ProgramData\obs-studio\plugins\`. You should end up with `C:\ProgramData\obs-studio\plugins\obs-discord-golive\bin\64bit\obs-discord-golive.dll`.
4. Start OBS. Open the panel from **Docks → Discord Go Live**.

## Use it

1. **Get the spare account's token.** Log in to the spare account at [discord.com/app](https://discord.com/app) in a web browser, open the developer tools (F12 or ⌥⌘I) → **Network** tab, click any channel, select a request to `discord.com/api`, and copy the value of its **Authorization** request header.
   Treat the token like a password: anyone who has it controls the account.
2. Paste it into **Account token** and press **Connect**. After a few seconds the status shows *Connected as <name>*.
3. Choose a **Voice channel** from the list.
4. Choose a **Stream quality**. Without Nitro, use **720p, 30 fps**.
5. Press **Start Discord stream**. The status shows *Live on Discord in …*. Join the voice channel from another account to watch.
6. Press **Stop Discord stream** when you're done. **Disconnect** logs the account out of the plugin.

| Button | When you see it |
|---|---|
| **Connect** | Not connected yet |
| **Disconnect** | While connecting (cancels), or when connected |
| **Refresh** | Connected; reloads the channel list |
| **Stop streaming in …** | The account is already live somewhere else. Stop that first, then Start appears |

### Stream quality

| Choice | Video | Needs Nitro |
|---|---|---|
| 720p, 30 fps | 3,000 Kbps | No |
| 1080p, 30 fps | 6,000 Kbps | Yes |
| 1080p, 60 fps | 8,000 Kbps | Yes |
| Same as OBS | Follows your OBS output size and frame rate (up to 8,000 Kbps) | Above 720p30, yes |

The plugin never upscales: if OBS outputs 720p, choosing 1080p still streams 720p. Audio is always 128 Kbps Opus.

## Troubleshooting

| Message or symptom | What to do |
|---|---|
| *Node.js was not found* | Install Node.js 22.18 or newer from nodejs.org, then restart OBS |
| *The token was rejected; check the account token* | The token is wrong or expired (it changes when you log out or change the password). Copy it again |
| *This account cannot stream in any voice channel* | The account needs permission to view, connect and stream (Video) in at least one voice channel |
| Viewers see audio but the video freezes or spins | Choose a lower **Stream quality** (720p, 30 fps), and check your upload speed |
| *The plugin's helper is missing* | Reinstall the plugin |
| No **Discord Go Live** under Docks | The plugin did not load. Check it is in the plugins folder above, and that your OBS is version 31 or newer |

For details, open **Help → Log Files → View Current Log** in OBS and search for `obs-discord-golive`. The token never appears in the log, so you can share it when asking for help.

## Uninstall

- **macOS:** delete `~/Library/Application Support/obs-studio/plugins/obs-discord-golive.plugin`.
- **Windows:** delete `C:\ProgramData\obs-studio\plugins\obs-discord-golive`.

The saved token and settings are in the OBS plugin settings folder (`plugin_config/obs-discord-golive`) inside your OBS configuration folder: `~/Library/Application Support/obs-studio` on macOS, `%APPDATA%\obs-studio` on Windows. Delete that folder to remove them too.

## For developers

The plugin is C/C++ (OBS plugin template, OBS 31.1.1). It sends OBS's encoded video and audio to a small Node.js/TypeScript helper (`helper/`), which talks to Discord with [`@dank074/discord-video-stream`](https://github.com/dank074/Discord-video-stream).

```bash
(cd helper && npm ci)              # helper dependencies
cmake --preset macos               # or: windows-x64
cmake --build --preset macos
scripts/release-macos.sh           # macOS release: .pkg and .zip in release/
node scripts/version.mjs patch     # bump the version
```

See [GUIDELINE.md](GUIDELINE.md) for the design, the pipe protocol, testing and the build details.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
