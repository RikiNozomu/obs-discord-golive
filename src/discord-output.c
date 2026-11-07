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
 * (H.264 video, Opus audio) and forwards them to the helper program over
 * its stdin, using the framing in GUIDELINE.md §4.
 *
 * Threads:
 *   OBS encoder thread  discord_output_packet: frames the packet and queues
 *                       it. Never blocks.
 *   worker              starts the helper, sends CONFIG, waits for READY,
 *                       begins data capture, then writes queued frames until
 *                       stop or failure, and shuts the helper down.
 *   stderr reader       reads the helper's status lines (READY, ERROR, LOG,
 *                       STATS) and forwards them to the OBS log.
 */

#include <stdlib.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "discord-ids.h"
#include "helper-process.h"
#include "plugin-support.h"

#define PROTOCOL_VERSION 1
#define FRAME_HEADER_SIZE 16

enum frame_type {
	FRAME_CONFIG = 1,
	FRAME_VIDEO = 2,
	FRAME_AUDIO = 3,
	FRAME_STOP = 4,
};

#define FRAME_FLAG_KEYFRAME 0x01

/* Longer than the helper's own login + join + Go Live timeouts (35 s), so its error wins. */
#define READY_TIMEOUT_MS 60000
#define EXIT_TIMEOUT_MS 3000

/* Drop video when the helper falls this far behind (GUIDELINE.md §3.3). */
#define MAX_VIDEO_LAG_US 700000
/* Hard cap on queued bytes, whatever the timestamps say. */
#define MAX_QUEUED_BYTES (64 * 1024 * 1024)

#define MAX_HELPER_ARGS 5

/* One frame ready to write: header and payload in one block. */
struct frame {
	struct frame *next;
	enum frame_type type;
	int64_t pts_us;
	size_t size;
	uint8_t data[];
};

struct discord_output {
	obs_output_t *output;

	/* Copied from the service settings at start. */
	char *token_file;
	char *guild_id;
	char *channel_id;
	char *helper_argv[MAX_HELPER_ARGS + 1];

	pthread_t worker;
	bool worker_valid;
	struct helper_process *helper;

	/* Set while data capture runs; gates discord_output_packet. */
	volatile bool active;
	volatile bool stop_requested;

	/* stderr reader */
	pthread_t reader;
	bool reader_valid;
	os_event_t *ready_event;
	volatile bool helper_ready;
	volatile bool helper_exited;
	pthread_mutex_t error_mutex;
	char *helper_error;

	/* Queue between the encoder thread and the worker. */
	pthread_mutex_t queue_mutex;
	os_sem_t *queue_sem;
	struct frame *head;
	struct frame *tail;
	size_t queued_bytes;
	bool have_video_ts;
	int64_t newest_video_us; /* last video frame queued */
	int64_t written_video_us; /* last video frame written */
	bool drop_until_keyframe;

	volatile long dropped_frames;
	uint64_t total_bytes;
};

/* ------------------------------------------------------------- helpers */

static const char *discord_output_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DiscordGoLive.Output");
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/* Payload is prefix (may be empty) followed by body. */
static struct frame *frame_create(enum frame_type type, uint8_t flags, int64_t pts_us, const uint8_t *prefix,
				  size_t prefix_size, const uint8_t *body, size_t body_size)
{
	size_t payload = prefix_size + body_size;
	struct frame *f = bmalloc(sizeof(*f) + FRAME_HEADER_SIZE + payload);

	f->next = NULL;
	f->type = type;
	f->pts_us = pts_us;
	f->size = FRAME_HEADER_SIZE + payload;

	f->data[0] = (uint8_t)type;
	f->data[1] = flags;
	put_le16(f->data + 2, 0);
	put_le64(f->data + 4, (uint64_t)pts_us);
	put_le32(f->data + 12, (uint32_t)payload);

	if (prefix_size)
		memcpy(f->data + FRAME_HEADER_SIZE, prefix, prefix_size);
	if (body_size)
		memcpy(f->data + FRAME_HEADER_SIZE + prefix_size, body, body_size);
	return f;
}

/*
 * Discord tokens look like <24+ chars>.<6 chars>.<27+ chars> of [A-Za-z0-9_-].
 * Replaces each one in place with asterisks (GUIDELINE.md §6).
 */
static bool is_token_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static size_t token_run(const char *s)
{
	size_t n = 0;
	while (is_token_char(s[n]))
		n++;
	return n;
}

static void redact_tokens(char *s)
{
	for (char *p = s; *p;) {
		size_t a = token_run(p);
		if (!a) {
			p++;
			continue;
		}

		size_t b = p[a] == '.' ? token_run(p + a + 1) : 0;
		size_t c = (b == 6 && p[a + 1 + b] == '.') ? token_run(p + a + 2 + b) : 0;

		if (a >= 24 && b == 6 && c >= 27) {
			size_t len = a + b + c + 2;
			memset(p, '*', len);
			p += len;
		} else {
			p += a;
		}
	}
}

static void set_helper_error(struct discord_output *dout, const char *msg)
{
	pthread_mutex_lock(&dout->error_mutex);
	bfree(dout->helper_error);
	dout->helper_error = bstrdup(msg);
	pthread_mutex_unlock(&dout->error_mutex);
}

/* Reports a failure to the user: the helper's own ERROR message if it sent one. */
static void set_last_error(struct discord_output *dout, const char *fallback_key)
{
	pthread_mutex_lock(&dout->error_mutex);
	if (dout->helper_error && *dout->helper_error)
		obs_output_set_last_error(dout->output, dout->helper_error);
	else
		obs_output_set_last_error(dout->output, obs_module_text(fallback_key));
	pthread_mutex_unlock(&dout->error_mutex);
}

/* --------------------------------------------------------------- queue */

static void queue_push(struct discord_output *dout, struct frame *f)
{
	pthread_mutex_lock(&dout->queue_mutex);
	if (dout->tail)
		dout->tail->next = f;
	else
		dout->head = f;
	dout->tail = f;
	dout->queued_bytes += f->size;
	pthread_mutex_unlock(&dout->queue_mutex);

	os_sem_post(dout->queue_sem);
}

static struct frame *queue_pop(struct discord_output *dout)
{
	pthread_mutex_lock(&dout->queue_mutex);
	struct frame *f = dout->head;
	if (f) {
		dout->head = f->next;
		if (!dout->head)
			dout->tail = NULL;
		dout->queued_bytes -= f->size;
		if (f->type == FRAME_VIDEO)
			dout->written_video_us = f->pts_us;
	}
	pthread_mutex_unlock(&dout->queue_mutex);
	return f;
}

static void queue_clear(struct discord_output *dout)
{
	pthread_mutex_lock(&dout->queue_mutex);
	struct frame *f = dout->head;
	while (f) {
		struct frame *next = f->next;
		bfree(f);
		f = next;
	}
	dout->head = dout->tail = NULL;
	dout->queued_bytes = 0;
	dout->have_video_ts = false;
	dout->drop_until_keyframe = false;
	pthread_mutex_unlock(&dout->queue_mutex);
}

/* ------------------------------------------------------- stderr reader */

static void handle_helper_line(struct discord_output *dout, char *line)
{
	size_t len = strlen(line);
	if (len && line[len - 1] == '\r')
		line[len - 1] = 0;
	if (!*line)
		return;

	redact_tokens(line);

	if (strcmp(line, "READY") == 0) {
		obs_log(LOG_INFO, "[discord-helper] ready");
		os_atomic_set_bool(&dout->helper_ready, true);
		os_event_signal(dout->ready_event);
	} else if (strncmp(line, "ERROR ", 6) == 0) {
		obs_log(LOG_WARNING, "[discord-helper] error: %s", line + 6);
		set_helper_error(dout, line + 6);
	} else if (strncmp(line, "LOG ", 4) == 0) {
		obs_log(LOG_INFO, "[discord-helper] %s", line + 4);
	} else if (strncmp(line, "STATS ", 6) == 0) {
		obs_log(LOG_INFO, "[discord-helper] stats %s", line + 6);
	} else {
		/* Anything else: Node warnings, stack traces. */
		obs_log(LOG_INFO, "[discord-helper] %s", line);
	}
}

static void *reader_thread(void *data)
{
	struct discord_output *dout = data;
	struct dstr line = {0};
	char buf[4096];
	size_t n;

	os_set_thread_name("discord-helper-stderr");

	while ((n = helper_process_read_err(dout->helper, buf, sizeof(buf))) > 0) {
		for (size_t i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				if (line.array)
					handle_helper_line(dout, line.array);
				dstr_free(&line);
			} else if (line.len < 64 * 1024) {
				dstr_ncat(&line, buf + i, 1);
			}
		}
	}
	if (line.array)
		handle_helper_line(dout, line.array);
	dstr_free(&line);

	/* The helper closed stderr, so it has exited or is about to. Wake whoever waits. */
	os_atomic_set_bool(&dout->helper_exited, true);
	os_event_signal(dout->ready_event);
	os_sem_post(dout->queue_sem);
	return NULL;
}

/* ------------------------------------------------------ helper control */

static char *build_config_json(struct discord_output *dout)
{
	obs_encoder_t *venc = obs_output_get_video_encoder(dout->output);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(dout->output, 0);
	const struct video_output_info *voi = video_output_get_info(obs_encoder_video(venc));
	obs_data_t *vsettings = obs_encoder_get_settings(venc);

	obs_data_t *video = obs_data_create();
	obs_data_set_string(video, "codec", "H264");
	obs_data_set_int(video, "width", obs_encoder_get_width(venc));
	obs_data_set_int(video, "height", obs_encoder_get_height(venc));
	if (voi && voi->fps_den && voi->fps_num % voi->fps_den == 0)
		obs_data_set_int(video, "fps", voi->fps_num / voi->fps_den);
	else if (voi && voi->fps_den)
		obs_data_set_double(video, "fps", (double)voi->fps_num / voi->fps_den);
	obs_data_set_int(video, "bitrateKbps", obs_data_get_int(vsettings, "bitrate"));
	obs_data_release(vsettings);

	obs_data_t *audio = obs_data_create();
	obs_data_set_string(audio, "codec", "opus");
	obs_data_set_int(audio, "sampleRate", obs_encoder_get_sample_rate(aenc));
	obs_data_set_int(audio, "channels", (long long)audio_output_get_channels(obs_encoder_audio(aenc)));

	obs_data_t *cfg = obs_data_create();
	obs_data_set_int(cfg, "v", PROTOCOL_VERSION);
	obs_data_set_string(cfg, "tokenFile", dout->token_file);
	obs_data_set_string(cfg, "guildId", dout->guild_id);
	obs_data_set_string(cfg, "channelId", dout->channel_id);
	obs_data_set_obj(cfg, "video", video);
	obs_data_set_obj(cfg, "audio", audio);

	char *json = bstrdup(obs_data_get_json(cfg));

	obs_data_release(video);
	obs_data_release(audio);
	obs_data_release(cfg);
	return json;
}

static bool write_frame(struct discord_output *dout, struct frame *f)
{
	if (!helper_process_write(dout->helper, f->data, f->size))
		return false;
	dout->total_bytes += f->size;
	return true;
}

static bool connect_helper(struct discord_output *dout)
{
	dout->helper = helper_process_start((const char *const *)dout->helper_argv);
	if (!dout->helper) {
		obs_log(LOG_WARNING, "could not start helper '%s'", dout->helper_argv[0]);
		obs_output_set_last_error(dout->output, obs_module_text("DiscordGoLive.Error.HelperStart"));
		return false;
	}

	obs_log(LOG_INFO, "started helper: %s%s%s", dout->helper_argv[0], dout->helper_argv[1] ? " " : "",
		dout->helper_argv[1] ? dout->helper_argv[1] : "");

	if (pthread_create(&dout->reader, NULL, reader_thread, dout) != 0) {
		obs_output_set_last_error(dout->output, obs_module_text("DiscordGoLive.Error.HelperStart"));
		return false;
	}
	dout->reader_valid = true;

	char *json = build_config_json(dout);
	struct frame *config = frame_create(FRAME_CONFIG, 0, 0, NULL, 0, (const uint8_t *)json, strlen(json));
	bool sent = write_frame(dout, config);
	bfree(config);
	bfree(json);

	if (sent)
		os_event_timedwait(dout->ready_event, READY_TIMEOUT_MS);

	if (os_atomic_load_bool(&dout->stop_requested))
		return false;

	if (!os_atomic_load_bool(&dout->helper_ready)) {
		bool exited = os_atomic_load_bool(&dout->helper_exited) || !sent;
		set_last_error(dout, exited ? "DiscordGoLive.Error.HelperExited" : "DiscordGoLive.Error.HelperTimeout");
		return false;
	}
	return true;
}

/* Asks the helper to finish, then makes sure it is gone. */
static void shutdown_helper(struct discord_output *dout, bool send_stop)
{
	if (!dout->helper)
		return;

	if (send_stop) {
		struct frame *stop = frame_create(FRAME_STOP, 0, 0, NULL, 0, NULL, 0);
		write_frame(dout, stop);
		bfree(stop);
	}
	helper_process_close_stdin(dout->helper);

	int code = 0;
	if (helper_process_wait(dout->helper, EXIT_TIMEOUT_MS, &code))
		obs_log(LOG_INFO, "helper exited with code %d", code);
	else
		obs_log(LOG_WARNING, "helper did not exit within %d ms, killing it", EXIT_TIMEOUT_MS);

	/* Once the helper is dead its stderr hits EOF and the reader returns. */
	helper_process_kill(dout->helper);
	if (dout->reader_valid) {
		pthread_join(dout->reader, NULL);
		dout->reader_valid = false;
	}
	helper_process_destroy(dout->helper);
	dout->helper = NULL;
}

/* -------------------------------------------------------------- worker */

static void *worker_thread(void *data)
{
	struct discord_output *dout = data;
	os_set_thread_name("discord-output");

	if (!connect_helper(dout)) {
		shutdown_helper(dout, false);
		if (os_atomic_load_bool(&dout->stop_requested)) {
			obs_output_signal_stop(dout->output, OBS_OUTPUT_SUCCESS);
		} else {
			obs_log(LOG_WARNING, "could not connect: %s", obs_output_get_last_error(dout->output));
			obs_output_signal_stop(dout->output, OBS_OUTPUT_CONNECT_FAILED);
		}
		return NULL;
	}

	os_atomic_set_bool(&dout->active, true);
	if (!obs_output_begin_data_capture(dout->output, 0)) {
		os_atomic_set_bool(&dout->active, false);
		shutdown_helper(dout, true);
		obs_output_set_last_error(dout->output, obs_module_text("DiscordGoLive.Error.Encoders"));
		obs_output_signal_stop(dout->output, OBS_OUTPUT_ERROR);
		return NULL;
	}
	obs_log(LOG_INFO, "streaming to the helper");

	bool disconnected = false;
	while (!disconnected) {
		os_sem_wait(dout->queue_sem);

		if (os_atomic_load_bool(&dout->stop_requested))
			break;
		if (os_atomic_load_bool(&dout->helper_exited)) {
			disconnected = true;
			break;
		}

		struct frame *f = queue_pop(dout);
		if (f && !write_frame(dout, f))
			disconnected = true;
		bfree(f);
	}

	os_atomic_set_bool(&dout->active, false);
	shutdown_helper(dout, !disconnected);
	queue_clear(dout);

	if (disconnected && !os_atomic_load_bool(&dout->stop_requested)) {
		set_last_error(dout, "DiscordGoLive.Error.HelperExited");
		obs_log(LOG_WARNING, "helper went away: %s", obs_output_get_last_error(dout->output));
		obs_output_signal_stop(dout->output, OBS_OUTPUT_DISCONNECTED);
	} else {
		obs_log(LOG_INFO, "stopped");
		obs_output_end_data_capture(dout->output);
	}
	return NULL;
}

static void join_worker(struct discord_output *dout)
{
	if (dout->worker_valid) {
		pthread_join(dout->worker, NULL);
		dout->worker_valid = false;
	}
}

/* -------------------------------------------------------- obs callbacks */

static void *discord_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct discord_output *dout = bzalloc(sizeof(*dout));
	dout->output = output;

	pthread_mutex_init_value(&dout->queue_mutex);
	pthread_mutex_init_value(&dout->error_mutex);
	if (pthread_mutex_init(&dout->queue_mutex, NULL) != 0 || pthread_mutex_init(&dout->error_mutex, NULL) != 0 ||
	    os_sem_init(&dout->queue_sem, 0) != 0 || os_event_init(&dout->ready_event, OS_EVENT_TYPE_MANUAL) != 0) {
		obs_log(LOG_ERROR, "could not create output synchronization objects");
	}
	return dout;
}

static void free_settings(struct discord_output *dout)
{
	bfree(dout->token_file);
	bfree(dout->guild_id);
	bfree(dout->channel_id);
	dout->token_file = dout->guild_id = dout->channel_id = NULL;

	for (size_t i = 0; i < MAX_HELPER_ARGS; i++) {
		bfree(dout->helper_argv[i]);
		dout->helper_argv[i] = NULL;
	}
}

static void discord_output_destroy(void *data)
{
	struct discord_output *dout = data;

	if (dout->worker_valid) {
		os_atomic_set_bool(&dout->stop_requested, true);
		os_event_signal(dout->ready_event);
		os_sem_post(dout->queue_sem);
		join_worker(dout);
	}

	queue_clear(dout);
	free_settings(dout);
	bfree(dout->helper_error);
	os_event_destroy(dout->ready_event);
	os_sem_destroy(dout->queue_sem);
	pthread_mutex_destroy(&dout->queue_mutex);
	pthread_mutex_destroy(&dout->error_mutex);
	bfree(dout);
}

static bool fail(struct discord_output *dout, const char *msg_key)
{
	const char *msg = obs_module_text(msg_key);

	obs_output_set_last_error(dout->output, msg);
	obs_log(LOG_WARNING, "output start failed: %s", msg);
	return false;
}

static bool has_suffix(const char *s, const char *suffix)
{
	size_t len = strlen(s), slen = strlen(suffix);
	return len >= slen && astrcmpi(s + len - slen, suffix) == 0;
}

static bool is_script(const char *path)
{
	static const char *exts[] = {".js", ".mjs", ".cjs", ".ts", ".mts"};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (has_suffix(path, exts[i]))
			return true;
	}
	return false;
}

/*
 * Works out the helper command line. The helper is either a standalone
 * executable (the packaged helper, M5) or a .ts/.js script run with Node,
 * which is how it runs during development.
 */
const char *discord_resolve_helper(const char *helper_path, char **argv, size_t *argc)
{
	char *helper = NULL;
	*argc = 0;

	/*
	 * Users always get the helper shipped inside the plugin. Developers can
	 * point at a working copy with the service setting (tests) or the
	 * OBS_DISCORD_GOLIVE_HELPER environment variable.
	 */
	const char *override_path = getenv("OBS_DISCORD_GOLIVE_HELPER");
	if (helper_path && *helper_path) {
		helper = bstrdup(helper_path);
	} else if (override_path && *override_path) {
		helper = bstrdup(override_path);
	} else {
		/* A single-file build (M5) wins; otherwise the bundled sources run with Node. */
#ifdef _WIN32
		helper = obs_module_file("helper/discord-helper.exe");
#else
		helper = obs_module_file("helper/discord-helper");
#endif
		if (!helper)
			helper = obs_module_file("helper/src/index.ts");
	}

	if (!helper || !os_file_exists(helper)) {
		bfree(helper);
		return "DiscordGoLive.Error.HelperNotFound";
	}

	if (is_script(helper)) {
		char *node = helper_process_find_executable("node");
		if (!node) {
			bfree(helper);
			return "DiscordGoLive.Error.NodeNotFound";
		}
		argv[(*argc)++] = node;
	}
	argv[(*argc)++] = helper;
	argv[*argc] = NULL;
	return NULL;
}

static const char *resolve_helper(struct discord_output *dout, const char *helper_path, const char *mock_dir)
{
	size_t argc = 0;
	const char *error = discord_resolve_helper(helper_path, dout->helper_argv, &argc);
	if (error)
		return error;

	if (mock_dir && *mock_dir) {
		dout->helper_argv[argc++] = bstrdup("--mock");
		dout->helper_argv[argc++] = bstrdup(mock_dir);
	}
	dout->helper_argv[argc] = NULL;
	return NULL;
}

static bool discord_output_start(void *data)
{
	struct discord_output *dout = data;
	obs_service_t *service = obs_output_get_service(dout->output);

	/* A reconnect starts the output again; the previous worker has finished by now. */
	join_worker(dout);

	if (!service)
		return fail(dout, "DiscordGoLive.Error.NoService");

	free_settings(dout);

	obs_data_t *settings = obs_service_get_settings(service);
	const char *token_file = obs_data_get_string(settings, S_TOKEN_FILE);
	bool token_ok = token_file && *token_file && os_file_exists(token_file);

	dout->token_file = bstrdup(token_file);
	dout->guild_id = bstrdup(obs_data_get_string(settings, S_GUILD_ID));
	dout->channel_id = bstrdup(obs_data_get_string(settings, S_CHANNEL_ID));

	const char *helper_error = token_ok ? resolve_helper(dout, obs_data_get_string(settings, S_HELPER_PATH),
							     obs_data_get_string(settings, S_MOCK_DIR))
					    : NULL;
	obs_data_release(settings);

	if (!token_ok)
		return fail(dout, "DiscordGoLive.Error.TokenFile");
	if (!obs_output_can_begin_data_capture(dout->output, 0))
		return fail(dout, "DiscordGoLive.Error.Encoders");
	if (helper_error)
		return fail(dout, helper_error);
	if (!obs_output_initialize_encoders(dout->output, 0))
		return fail(dout, "DiscordGoLive.Error.Encoders");

	queue_clear(dout);
	set_helper_error(dout, NULL);
	os_event_reset(dout->ready_event);
	/* No worker is running, so the semaphore can be swapped for a fresh one (count 0). */
	os_sem_destroy(dout->queue_sem);
	if (os_sem_init(&dout->queue_sem, 0) != 0)
		return fail(dout, "DiscordGoLive.Error.HelperStart");
	os_atomic_set_bool(&dout->stop_requested, false);
	os_atomic_set_bool(&dout->helper_ready, false);
	os_atomic_set_bool(&dout->helper_exited, false);
	os_atomic_set_long(&dout->dropped_frames, 0);
	dout->total_bytes = 0;

	if (pthread_create(&dout->worker, NULL, worker_thread, dout) != 0)
		return fail(dout, "DiscordGoLive.Error.HelperStart");
	dout->worker_valid = true;
	return true;
}

static void discord_output_stop(void *data, uint64_t ts)
{
	struct discord_output *dout = data;
	UNUSED_PARAMETER(ts);

	if (!dout->worker_valid)
		return;

	/* The worker notices, shuts the helper down and ends data capture. */
	os_atomic_set_bool(&dout->stop_requested, true);
	os_event_signal(dout->ready_event);
	os_sem_post(dout->queue_sem);
}

/* True if the Annex-B data already carries an SPS (NAL type 7). */
static bool has_sps(const uint8_t *data, size_t size)
{
	for (size_t i = 0; i + 3 < size; i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 7)
			return true;
	}
	return false;
}

static int64_t packet_pts_us(const struct encoder_packet *packet)
{
	if (!packet->timebase_den)
		return 0;

	/*
	 * With a frame rate divisor (the dock's "Stream quality" dropping 60 -> 30
	 * fps), libobs 31 counts video pts in canvas frames (+divisor per encoded
	 * frame) but also multiplies the packet timebase by the divisor, so the
	 * plain product is divisor times too large. Undo the timebase multiply.
	 */
	int64_t num = packet->timebase_num;
	if (packet->type == OBS_ENCODER_VIDEO && packet->encoder) {
		uint32_t divisor = obs_encoder_get_frame_rate_divisor(packet->encoder);
		if (divisor > 1 && num % divisor == 0)
			num /= divisor;
	}
	return packet->pts * 1000000LL * num / packet->timebase_den;
}

static void drop_frame(struct discord_output *dout)
{
	os_atomic_inc_long(&dout->dropped_frames);
}

/* Runs on the encoder thread: must never block on the helper. */
static void discord_output_packet(void *data, struct encoder_packet *packet)
{
	struct discord_output *dout = data;

	/* A NULL packet means an encoder failed. */
	if (!packet) {
		obs_output_signal_stop(dout->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}
	if (!os_atomic_load_bool(&dout->active))
		return;

	int64_t pts_us = packet_pts_us(packet);
	bool video = packet->type == OBS_ENCODER_VIDEO;

	if (video) {
		pthread_mutex_lock(&dout->queue_mutex);
		if (!dout->have_video_ts) {
			dout->have_video_ts = true;
			dout->written_video_us = pts_us;
		}

		bool too_far_behind = pts_us - dout->written_video_us > MAX_VIDEO_LAG_US;
		bool too_big = dout->queued_bytes > MAX_QUEUED_BYTES;

		/* Once a frame is dropped, every frame up to the next keyframe depends on it. */
		if (dout->drop_until_keyframe && packet->keyframe && !too_big)
			dout->drop_until_keyframe = false;
		if (!dout->drop_until_keyframe && (too_big || (too_far_behind && !packet->keyframe))) {
			dout->drop_until_keyframe = true;
			obs_log(LOG_DEBUG, "helper is %lld ms behind, dropping video until the next keyframe",
				(long long)((pts_us - dout->written_video_us) / 1000));
		}

		bool drop = dout->drop_until_keyframe;
		if (!drop)
			dout->newest_video_us = pts_us;
		pthread_mutex_unlock(&dout->queue_mutex);

		if (drop) {
			drop_frame(dout);
			return;
		}

		/* Viewers who join late need SPS/PPS before each keyframe. */
		uint8_t *header = NULL;
		size_t header_size = 0;
		if (packet->keyframe && !has_sps(packet->data, packet->size) && packet->encoder)
			obs_encoder_get_extra_data(packet->encoder, &header, &header_size);

		queue_push(dout, frame_create(FRAME_VIDEO, packet->keyframe ? FRAME_FLAG_KEYFRAME : 0, pts_us, header,
					      header_size, packet->data, packet->size));
	} else {
		/* Audio is tiny and the helper's clock; only drop it if the queue is enormous. */
		if (dout->queued_bytes > MAX_QUEUED_BYTES) {
			drop_frame(dout);
			return;
		}
		queue_push(dout, frame_create(FRAME_AUDIO, 0, pts_us, NULL, 0, packet->data, packet->size));
	}
}

static uint64_t discord_output_total_bytes(void *data)
{
	struct discord_output *dout = data;
	return dout->total_bytes;
}

static int discord_output_dropped_frames(void *data)
{
	struct discord_output *dout = data;
	return (int)os_atomic_load_long(&dout->dropped_frames);
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
		.get_dropped_frames = discord_output_dropped_frames,
	};

	obs_register_output(&info);
}
