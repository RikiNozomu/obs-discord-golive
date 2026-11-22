/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * "Discord Go Live" filter: marks a source that the dock can stream on its
 * own, instead of the whole OBS output.
 *
 * The filter itself passes video and audio through untouched. When the dock
 * starts a stream for it (discord_filter_begin), it creates:
 *
 *   - an obs_view that renders the filter's parent source on a canvas of the
 *     source's size, with its own video_t for the encoder;
 *   - an audio_t whose input is the source's own audio, copied out of
 *     filter_audio into a small per-channel buffer.
 *
 * So only that source's picture and sound go to Discord, whatever the
 * current scene shows and whatever else is in the audio mix.
 */

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/deque.h>
#include <util/platform.h>
#include <util/threading.h>

#include "discord-filter.h"
#include "plugin-support.h"

/* How long to wait for a source that was not showing to report its size. */
#define SIZE_WAIT_MS 2000

/* Source audio kept waiting for the audio output; more is dropped (oldest first). */
#define MAX_BUFFERED_FRAMES (AUDIO_OUTPUT_FRAMES * 4)

struct discord_filter {
	obs_source_t *self;

	pthread_mutex_t mutex;
	bool capturing; /* guarded by mutex: filter_audio copies audio */
	size_t channels;
	struct deque audio_buf[MAX_AUDIO_CHANNELS];

	/* Only touched from the UI thread (begin/end/destroy). */
	obs_source_t *active_parent; /* parent we made active, with a reference */
	obs_view_t *view;
	video_t *video;
	audio_t *audio;
};

static const char *discord_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DiscordGoLive.Filter");
}

static void *discord_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct discord_filter *df = bzalloc(sizeof(*df));
	df->self = source;
	pthread_mutex_init_value(&df->mutex);
	if (pthread_mutex_init(&df->mutex, NULL) != 0) {
		bfree(df);
		return NULL;
	}
	return df;
}

static void clear_audio(struct discord_filter *df)
{
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		deque_free(&df->audio_buf[i]);
}

static void stop_capture(struct discord_filter *df)
{
	/* Order matters: nothing may pull audio or render after its owner is gone. */
	if (df->audio) {
		audio_output_close(df->audio);
		df->audio = NULL;
	}
	if (df->view) {
		obs_view_remove(df->view);
		obs_view_set_source(df->view, 0, NULL);
		obs_view_destroy(df->view);
		df->view = NULL;
	}
	df->video = NULL;
	if (df->active_parent) {
		obs_source_dec_active(df->active_parent);
		obs_source_release(df->active_parent);
		df->active_parent = NULL;
	}

	pthread_mutex_lock(&df->mutex);
	df->capturing = false;
	clear_audio(df);
	pthread_mutex_unlock(&df->mutex);
}

static void discord_filter_destroy(void *data)
{
	struct discord_filter *df = data;

	stop_capture(df);
	pthread_mutex_destroy(&df->mutex);
	bfree(df);
}

static obs_properties_t *discord_filter_properties(void *data)
{
	struct discord_filter *df = data;
	return discord_filter_build_properties(df->self);
}

static void discord_filter_update(void *data, obs_data_t *settings)
{
	struct discord_filter *df = data;
	UNUSED_PARAMETER(settings);
	discord_filter_settings_changed(df->self);
}

static void discord_filter_video_render(void *data, gs_effect_t *effect)
{
	struct discord_filter *df = data;
	UNUSED_PARAMETER(effect);
	obs_source_skip_video_filter(df->self);
}

/* OBS audio thread: copy the source's audio for our audio output, pass it on unchanged. */
static struct obs_audio_data *discord_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct discord_filter *df = data;

	pthread_mutex_lock(&df->mutex);
	if (df->capturing) {
		for (size_t ch = 0; ch < df->channels; ch++) {
			if (!audio->data[ch])
				continue;
			struct deque *buf = &df->audio_buf[ch];
			deque_push_back(buf, audio->data[ch], audio->frames * sizeof(float));
			while (buf->size > MAX_BUFFERED_FRAMES * sizeof(float))
				deque_pop_front(buf, NULL, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}
	pthread_mutex_unlock(&df->mutex);

	return audio;
}

/* Our audio output's thread: hand over one block of the source's audio, or silence. */
static bool audio_input(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts, uint32_t active_mixers,
			struct audio_output_data *mixes)
{
	struct discord_filter *df = param;
	UNUSED_PARAMETER(end_ts);
	UNUSED_PARAMETER(active_mixers);

	const size_t bytes = AUDIO_OUTPUT_FRAMES * sizeof(float);

	pthread_mutex_lock(&df->mutex);
	/* Mix buffers start zeroed, so skipping a channel leaves silence. */
	if (df->audio_buf[0].size >= bytes) {
		for (size_t ch = 0; ch < df->channels; ch++) {
			if (df->audio_buf[ch].size >= bytes)
				deque_pop_front(&df->audio_buf[ch], mixes[0].data[ch], bytes);
		}
	}
	pthread_mutex_unlock(&df->mutex);

	*new_ts = start_ts;
	return true;
}

const char *discord_filter_begin(obs_source_t *filter, video_t **video, audio_t **audio, uint32_t *width,
				 uint32_t *height)
{
	struct discord_filter *df = obs_obj_get_data(filter);
	obs_source_t *parent = obs_filter_get_parent(filter);
	if (!df || !parent)
		return "DiscordGoLive.Error.FilterSource";

	stop_capture(df);

	/*
	 * Make the source fully active, as if it were in the program: media files
	 * and similar sources only start playing then, so a source that is not
	 * in the current scene still streams. It reports its size once its first
	 * frame is in, so wait a moment for that.
	 */
	df->active_parent = obs_source_get_ref(parent);
	if (df->active_parent)
		obs_source_inc_active(df->active_parent);

	df->view = obs_view_create();
	obs_view_set_source(df->view, 0, parent);

	uint32_t w = 0, h = 0;
	for (int waited = 0; waited <= SIZE_WAIT_MS; waited += 50) {
		/* Encoders need even sizes. */
		w = obs_source_get_width(parent) & ~1u;
		h = obs_source_get_height(parent) & ~1u;
		if (w && h)
			break;
		os_sleep_ms(50);
	}
	if (!w || !h) {
		stop_capture(df);
		return "DiscordGoLive.Error.FilterNoVideo";
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		stop_capture(df);
		return "DiscordGoLive.Error.FilterSource";
	}
	ovi.base_width = ovi.output_width = w;
	ovi.base_height = ovi.output_height = h;

	df->video = obs_view_add2(df->view, &ovi);
	if (!df->video) {
		stop_capture(df);
		return "DiscordGoLive.Error.FilterSource";
	}

	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai)) {
		stop_capture(df);
		return "DiscordGoLive.Error.FilterSource";
	}

	pthread_mutex_lock(&df->mutex);
	df->channels = get_audio_channels(oai.speakers);
	df->capturing = true;
	pthread_mutex_unlock(&df->mutex);

	struct audio_output_info aoi = {
		.name = "discord golive source audio",
		.samples_per_sec = oai.samples_per_sec,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = oai.speakers,
		.input_callback = audio_input,
		.input_param = df,
	};
	if (audio_output_open(&df->audio, &aoi) != AUDIO_OUTPUT_SUCCESS) {
		df->audio = NULL;
		stop_capture(df);
		return "DiscordGoLive.Error.FilterSource";
	}

	obs_log(LOG_INFO, "streaming source '%s' on its own (%ux%u)", obs_source_get_name(parent), w, h);
	*video = df->video;
	*audio = df->audio;
	*width = w;
	*height = h;
	return NULL;
}

void discord_filter_end(obs_source_t *filter)
{
	struct discord_filter *df = filter ? obs_obj_get_data(filter) : NULL;
	if (df)
		stop_capture(df);
}

bool discord_filter_is(obs_source_t *source)
{
	return source && strcmp(obs_source_get_id(source), DISCORD_FILTER_ID) == 0;
}

void discord_register_filter(void)
{
	struct obs_source_info info = {
		.id = DISCORD_FILTER_ID,
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO,
		.get_name = discord_filter_name,
		.create = discord_filter_create,
		.destroy = discord_filter_destroy,
		.get_properties = discord_filter_properties,
		.update = discord_filter_update,
		.video_render = discord_filter_video_render,
		.filter_audio = discord_filter_audio,
	};

	obs_register_source(&info);
}
