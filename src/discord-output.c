/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * The Discord output receives packets that OBS has already encoded
 * (H.264 video, Opus audio).
 *
 * Milestone M1 (this file): registration, validation of the service settings
 * and token file, and a clean failure explaining that the streaming
 * pipeline is not there yet.
 * Milestone M2: spawn the helper, send the CONFIG frame, queue packets and
 * write them to the helper's stdin from a writer thread (see GUIDELINE.md §3.3, §4).
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "discord-ids.h"
#include "plugin-support.h"

struct discord_output {
	obs_output_t *output;
	volatile bool active;
	uint64_t total_bytes;
};

static const char *discord_output_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DiscordGoLive.Output");
}

static void *discord_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct discord_output *dout = bzalloc(sizeof(*dout));
	dout->output = output;
	return dout;
}

static void discord_output_destroy(void *data)
{
	bfree(data);
}

static bool fail(struct discord_output *dout, const char *msg_key)
{
	const char *msg = obs_module_text(msg_key);

	obs_output_set_last_error(dout->output, msg);
	obs_log(LOG_WARNING, "output start failed: %s", msg);
	return false;
}

static bool discord_output_start(void *data)
{
	struct discord_output *dout = data;
	obs_service_t *service = obs_output_get_service(dout->output);

	if (!service)
		return fail(dout, "DiscordGoLive.Error.NoService");

	obs_data_t *settings = obs_service_get_settings(service);
	const char *token_file = obs_data_get_string(settings, S_TOKEN_FILE);
	bool token_ok = token_file && *token_file && os_file_exists(token_file);
	obs_data_release(settings);

	if (!token_ok)
		return fail(dout, "DiscordGoLive.Error.TokenFile");

	if (!obs_output_can_begin_data_capture(dout->output, 0))
		return fail(dout, "DiscordGoLive.Error.Encoders");

	/* M2 starts the helper and begins data capture here. */
	return fail(dout, "DiscordGoLive.Error.NotImplemented");
}

static void discord_output_stop(void *data, uint64_t ts)
{
	struct discord_output *dout = data;
	UNUSED_PARAMETER(ts);

	if (os_atomic_set_bool(&dout->active, false))
		obs_output_end_data_capture(dout->output);
}

static void discord_output_packet(void *data, struct encoder_packet *packet)
{
	struct discord_output *dout = data;

	/* A NULL packet means an encoder failed. */
	if (!packet) {
		obs_output_signal_stop(dout->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	/* M2: queue the packet for the writer thread. */
	dout->total_bytes += packet->size;
}

static uint64_t discord_output_total_bytes(void *data)
{
	struct discord_output *dout = data;
	return dout->total_bytes;
}

void discord_register_output(void)
{
	struct obs_output_info info = {
		.id = DISCORD_OUTPUT_ID,
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
		.protocols = DISCORD_PROTOCOL,
		.encoded_video_codecs = "h264",
		.encoded_audio_codecs = "opus",
		.get_name = discord_output_name,
		.create = discord_output_create,
		.destroy = discord_output_destroy,
		.start = discord_output_start,
		.stop = discord_output_stop,
		.encoded_packet = discord_output_packet,
		.get_total_bytes = discord_output_total_bytes,
	};

	obs_register_output(&info);
}
