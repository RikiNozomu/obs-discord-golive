/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * The "Discord Go Live" service describes where the stream goes (server and
 * voice channel) and what the encoders must produce. OBS 31's Settings ->
 * Stream page only lists its built-in services, so this service is created
 * privately by the dock (discord-dock.cpp) rather than picked in Settings.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "discord-ids.h"
#include "plugin-support.h"

struct discord_service {
	obs_service_t *service;
	char *guild_id;
	char *channel_id;
	char *token_file;
	char *helper_path;
};

static const char *video_codecs[] = {"h264", NULL};
static const char *audio_codecs[] = {"opus", NULL};

static const char *discord_service_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DiscordGoLive.Service");
}

static void discord_service_update(void *data, obs_data_t *settings)
{
	struct discord_service *ds = data;

	bfree(ds->guild_id);
	bfree(ds->channel_id);
	bfree(ds->token_file);
	bfree(ds->helper_path);

	ds->guild_id = bstrdup(obs_data_get_string(settings, S_GUILD_ID));
	ds->channel_id = bstrdup(obs_data_get_string(settings, S_CHANNEL_ID));
	ds->token_file = bstrdup(obs_data_get_string(settings, S_TOKEN_FILE));
	ds->helper_path = bstrdup(obs_data_get_string(settings, S_HELPER_PATH));
}

static void *discord_service_create(obs_data_t *settings, obs_service_t *service)
{
	struct discord_service *ds = bzalloc(sizeof(*ds));
	ds->service = service;
	discord_service_update(ds, settings);
	return ds;
}

static void discord_service_destroy(void *data)
{
	struct discord_service *ds = data;

	bfree(ds->guild_id);
	bfree(ds->channel_id);
	bfree(ds->token_file);
	bfree(ds->helper_path);
	bfree(ds);
}

static obs_properties_t *discord_service_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, S_GUILD_ID, obs_module_text("DiscordGoLive.GuildId"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, S_CHANNEL_ID, obs_module_text("DiscordGoLive.ChannelId"), OBS_TEXT_DEFAULT);
	obs_properties_add_path(props, S_TOKEN_FILE, obs_module_text("DiscordGoLive.TokenFile"), OBS_PATH_FILE, NULL,
				NULL);
	obs_properties_add_path(props, S_HELPER_PATH, obs_module_text("DiscordGoLive.HelperPath"), OBS_PATH_FILE, NULL,
				NULL);

	return props;
}

/* Discord snowflake IDs are 17-20 decimal digits. */
static bool is_snowflake(const char *s)
{
	size_t len = s ? strlen(s) : 0;

	if (len < 17 || len > 20)
		return false;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
	}
	return true;
}

static bool discord_service_can_try_to_connect(void *data)
{
	struct discord_service *ds = data;

	return is_snowflake(ds->guild_id) && is_snowflake(ds->channel_id) && ds->token_file && *ds->token_file;
}

/*
 * Settings that keep real-time RTP playback smooth on Discord:
 * no B-frames, frequent keyframes, constant bitrate, 48 kHz Opus.
 */
static void discord_service_apply_encoder_settings(void *data, obs_data_t *video, obs_data_t *audio)
{
	UNUSED_PARAMETER(data);

	if (video) {
		obs_data_set_int(video, "bf", 0);
		obs_data_set_string(video, "rate_control", "CBR");
		obs_data_set_string(video, "profile", "main");
		obs_data_set_string(video, "tune", "zerolatency");
		obs_data_set_bool(video, "repeat_headers", true);
		if (obs_data_get_int(video, "keyint_sec") <= 0 || obs_data_get_int(video, "keyint_sec") > 2)
			obs_data_set_int(video, "keyint_sec", DEFAULT_KEYINT_SEC);
	}

	if (audio) {
		int64_t br = obs_data_get_int(audio, "bitrate");
		if (br <= 0 || br > 256)
			obs_data_set_int(audio, "bitrate", DEFAULT_AUDIO_BITRATE);
	}
}

static const char *discord_service_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return DISCORD_OUTPUT_ID;
}

static const char *discord_service_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return DISCORD_PROTOCOL;
}

static const char **discord_service_video_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	return video_codecs;
}

static const char **discord_service_audio_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	return audio_codecs;
}

/* Exposes the voice channel as the "server URL" for anything that logs it. */
static const char *discord_service_connect_info(void *data, uint32_t type)
{
	struct discord_service *ds = data;

	if (type == OBS_SERVICE_CONNECT_INFO_SERVER_URL)
		return ds->channel_id;
	return NULL;
}

void discord_register_service(void)
{
	struct obs_service_info info = {
		.id = DISCORD_SERVICE_ID,
		.get_name = discord_service_name,
		.create = discord_service_create,
		.destroy = discord_service_destroy,
		.update = discord_service_update,
		.get_properties = discord_service_properties,
		.can_try_to_connect = discord_service_can_try_to_connect,
		.apply_encoder_settings = discord_service_apply_encoder_settings,
		.get_output_type = discord_service_output_type,
		.get_protocol = discord_service_protocol,
		.get_supported_video_codecs = discord_service_video_codecs,
		.get_supported_audio_codecs = discord_service_audio_codecs,
		.get_connect_info = discord_service_connect_info,
	};

	obs_register_service(&info);
}
