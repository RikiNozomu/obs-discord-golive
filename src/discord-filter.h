/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISCORD_FILTER_ID "discord_golive_filter"

void discord_register_filter(void);

/* True if source is a "Discord Go Live" filter. */
bool discord_filter_is(obs_source_t *source);

/*
 * Starts rendering the filter's source on its own canvas (its current size)
 * and capturing its own audio. On success returns NULL and the video/audio to
 * give the encoders; otherwise a locale key for the error. UI thread only.
 * Release the encoders before calling discord_filter_end().
 */
const char *discord_filter_begin(obs_source_t *filter, video_t **video, audio_t **audio, uint32_t *width,
				 uint32_t *height);

/* Stops what discord_filter_begin started. Safe to call twice. */
void discord_filter_end(obs_source_t *filter);

/*
 * Defined in discord-dock.cpp: the filter's controls are its OBS properties,
 * driven by a hidden per-filter controller.
 */
obs_properties_t *discord_filter_build_properties(obs_source_t *filter);
void discord_filter_settings_changed(obs_source_t *filter);

#ifdef __cplusplus
}
#endif
