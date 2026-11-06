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

/* Service settings keys. The token itself is never stored, only a path to it. */
#define S_GUILD_ID "guild_id"
#define S_CHANNEL_ID "channel_id"
#define S_TOKEN_FILE "token_file"
#define S_HELPER_PATH "helper_path"

/* Dock-only settings keys (encoder choices). */
#define S_VIDEO_BITRATE "video_bitrate"
#define S_AUDIO_BITRATE "audio_bitrate"
#define S_KEYINT_SEC "keyint_sec"

#define DEFAULT_VIDEO_BITRATE 4000
#define DEFAULT_AUDIO_BITRATE 128
#define DEFAULT_KEYINT_SEC 2

#ifdef __cplusplus
extern "C" {
#endif

void discord_register_service(void);
void discord_register_output(void);

#ifdef __cplusplus
}
#endif
