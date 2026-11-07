/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

/* Object type IDs registered with libobs. */
#define DISCORD_SERVICE_ID "discord_golive_service"
#define DISCORD_OUTPUT_ID "discord_golive_output"

/* Custom protocol shared by the service and the output. */
#define DISCORD_PROTOCOL "DISCORD"

/*
 * Service settings keys. The token itself is never stored in settings, only a
 * path to it; the dock writes the typed token to an owner-only file.
 */
#define S_GUILD_ID "guild_id"
#define S_CHANNEL_ID "channel_id"
#define S_TOKEN_FILE "token_file"
/* Developer/test override only; users always run the helper bundled with the plugin. */
#define S_HELPER_PATH "helper_path"
/* Test mode: when set, the helper dumps the stream into this folder instead of going to Discord. */
#define S_MOCK_DIR "mock_dir"

/* Dock-only settings keys (encoder choices). */
#define S_KEYINT_SEC "keyint_sec"

#define S_QUALITY "quality"

#define DEFAULT_VIDEO_BITRATE 4000
/*
 * The library announces max_bitrate 10 Mbps to Discord; sending more makes the
 * stream server drop video. Keep headroom for RTP/RTX overhead and pacing.
 */
#define MAX_VIDEO_BITRATE 8000
#define DEFAULT_AUDIO_BITRATE 128
#define DEFAULT_KEYINT_SEC 2

#ifdef __cplusplus
extern "C" {
#endif

void discord_register_service(void);
void discord_register_output(void);

/*
 * Fills argv with the command that runs the helper: [node] helper, then NULL.
 * argv needs room for 3 entries; the caller bfree()s each one. Returns NULL,
 * or a locale key describing why the helper cannot run.
 */
const char *discord_resolve_helper(const char *helper_path, char **argv, size_t *argc);

#ifdef __cplusplus
}
#endif
