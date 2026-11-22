/*
 * Headless end-to-end run on macOS: real OBS encoders -> discord output ->
 * helper in test mode (--mock), which writes video.h264, audio.opus and
 * report.json into <mock-dir> and checks them.
 *
 * Build (from the repo root, after cmake --preset macos):
 *   clang -o e2e tests/e2e-headless.c src/discord-filter.c -Isrc \
 *     -I.deps/obs-studio-31.1.1/libobs -I.deps/obs-studio-31.1.1/build_universal/config \
 *     -F/Applications/OBS.app/Contents/Frameworks -framework libobs \
 *     -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks
 *
 * Run:
 *   P=build_macos/RelWithDebInfo/obs-discord-golive.plugin
 *   ./e2e $P/Contents/MacOS/obs-discord-golive $P/Contents/Resources <token-file> <mock-dir> <seconds> [encoder-id|list]
 *
 * Environment:
 *   E2E_W, E2E_H, E2E_FPS   OBS canvas (default 1280x720 at 30)
 *   E2E_SW, E2E_SH          encoder scaled size
 *   E2E_DIV                 encoder frame rate divisor
 *   E2E_MEDIA=<file>        stream only this media file through the "Discord Go
 *                           Live" filter (the program output stays a blue colour)
 *   E2E_MEDIA2=<file>       also stream a second file through its own filter at the
 *                           same time; its dump goes to <mock-dir>-2
 *
 * The filter code is compiled in from src/discord-filter.c, because the
 * plugin binary does not export it.
 */

#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord-filter.h"

#define OBS_APP "/Applications/OBS.app/Contents"
#define PLUGIN(name) OBS_APP "/PlugIns/" name ".plugin/Contents/MacOS/" name, OBS_APP "/PlugIns/" name ".plugin/Contents/Resources"

/* What discord-filter.c needs from the plugin module. */
const char *obs_module_text(const char *key)
{
	return key;
}

void obs_log(int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("plugin-log(%d): ", level);
	vprintf(format, args);
	printf("\n");
	va_end(args);
}

obs_properties_t *discord_filter_build_properties(obs_source_t *filter)
{
	(void)filter;
	return obs_properties_create();
}

void discord_filter_settings_changed(obs_source_t *filter)
{
	(void)filter;
}

static bool load(const char *bin, const char *data)
{
	obs_module_t *m = NULL;
	if (obs_open_module(&m, bin, data) != MODULE_SUCCESS) {
		fprintf(stderr, "open %s failed\n", bin);
		return false;
	}
	return obs_init_module(m);
}

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);
	return v && *v ? atoi(v) : def;
}

struct stream {
	obs_source_t *media, *filter;
	obs_service_t *svc;
	obs_encoder_t *venc, *aenc;
	obs_output_t *out;
	os_event_t *stopped;
	volatile long code;
};

static void stream_stopped(void *d, calldata_t *cd)
{
	struct stream *st = d;
	os_atomic_set_long(&st->code, (long)calldata_int(cd, "code"));
	os_event_signal(st->stopped);
}

/* media = NULL streams the whole output; otherwise that file through a Discord Go Live filter. */
static bool start_stream(struct stream *st, const char *media, const char *token, const char *mock,
			 const char *venc_id)
{
	video_t *video = obs_get_video();
	audio_t *audio = obs_get_audio();

	if (media) {
		/* The plugin already registered the filter type; only its functions are compiled in here. */
		obs_data_t *ms = obs_data_create();
		obs_data_set_string(ms, "local_file", media);
		obs_data_set_bool(ms, "looping", true);
		st->media = obs_source_create("ffmpeg_source", media, ms, NULL);
		obs_data_release(ms);
		st->filter = obs_source_create(DISCORD_FILTER_ID, "discord", NULL, NULL);
		obs_source_filter_add(st->media, st->filter);

		uint32_t fw = 0, fh = 0;
		const char *err = discord_filter_begin(st->filter, &video, &audio, &fw, &fh);
		printf("FILTER begin %s: %s size=%ux%u\n", media, err ? err : "ok", fw, fh);
		if (err)
			return false;
	}

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "guild_id", "123456789012345678");
	obs_data_set_string(s, "channel_id", "987654321098765432");
	obs_data_set_string(s, "token_file", token);
	obs_data_set_string(s, "mock_dir", mock);
	st->svc = obs_service_create_private("discord_golive_service", "svc", s);
	obs_data_release(s);

	obs_data_t *v = obs_data_create(), *a = obs_data_create();
	obs_data_set_int(v, "bitrate", 3000);
	obs_data_set_string(v, "preset", "veryfast");
	obs_data_set_int(a, "bitrate", 128);
	obs_service_apply_encoder_settings(st->svc, v, a);
	st->venc = obs_video_encoder_create(venc_id, "v", v, NULL);
	st->aenc = obs_audio_encoder_create("ffmpeg_opus", "a", a, 0, NULL);
	obs_data_release(v);
	obs_data_release(a);
	if (!st->venc || !st->aenc)
		return false;
	obs_encoder_set_video(st->venc, video);
	obs_encoder_set_audio(st->aenc, audio);
	if (env_int("E2E_SW", 0))
		obs_encoder_set_scaled_size(st->venc, env_int("E2E_SW", 0), env_int("E2E_SH", 0));
	if (env_int("E2E_DIV", 1) > 1)
		obs_encoder_set_frame_rate_divisor(st->venc, env_int("E2E_DIV", 1));

	st->out = obs_output_create("discord_golive_output", "out", NULL, NULL);
	obs_output_set_video_encoder(st->out, st->venc);
	obs_output_set_audio_encoder(st->out, st->aenc, 0);
	obs_output_set_service(st->out, st->svc);
	obs_output_set_reconnect_settings(st->out, 0, 0);

	st->code = -999;
	os_event_init(&st->stopped, OS_EVENT_TYPE_MANUAL);
	signal_handler_connect(obs_output_get_signal_handler(st->out), "stop", stream_stopped, st);

	if (!obs_output_start(st->out)) {
		printf("RESULT start=false error=%s\n", obs_output_get_last_error(st->out));
		return false;
	}
	for (int i = 0; i < 300 && !obs_output_active(st->out) && os_event_try(st->stopped) == EAGAIN; i++)
		os_sleep_ms(100);
	return true;
}

static void stop_stream(struct stream *st, int index)
{
	if (os_event_try(st->stopped) == EAGAIN) {
		obs_output_stop(st->out);
		os_event_timedwait(st->stopped, 10000);
	}
	printf("RESULT[%d] code=%ld bytes=%llu dropped=%d error=%s\n", index, os_atomic_load_long(&st->code),
	       (unsigned long long)obs_output_get_total_bytes(st->out), obs_output_get_frames_dropped(st->out),
	       obs_output_get_last_error(st->out) ? obs_output_get_last_error(st->out) : "(none)");

	obs_output_release(st->out);
	obs_encoder_release(st->venc);
	obs_encoder_release(st->aenc);
	if (st->filter) {
		discord_filter_end(st->filter);
		obs_source_filter_remove(st->media, st->filter);
		obs_source_release(st->filter);
		obs_source_release(st->media);
		printf("FILTER[%d] ended\n", index);
	}
	obs_service_release(st->svc);
	os_event_destroy(st->stopped);
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s plugin-bin plugin-data token-file mock-dir seconds [encoder-id|list]\n",
			argv[0]);
		return 1;
	}
	const char *plugin = argv[1], *pdata = argv[2], *token = argv[3], *mock = argv[4];
	int seconds = atoi(argv[5]);
	const char *venc_id = argc > 6 ? argv[6] : "obs_x264";

	if (!obs_startup("en-US", NULL, NULL))
		return 10;

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-opengl.dylib";
	ovi.fps_num = env_int("E2E_FPS", 30);
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = env_int("E2E_W", 1280);
	ovi.base_height = ovi.output_height = env_int("E2E_H", 720);
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 11;
	struct obs_audio_info oai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&oai))
		return 12;

	if (!load(PLUGIN("obs-x264")) || !load(PLUGIN("obs-ffmpeg")) || !load(PLUGIN("image-source")) ||
	    !load(PLUGIN("mac-videotoolbox")) || !load(plugin, pdata))
		return 13;

	if (!strcmp(venc_id, "list")) {
		const char *id;
		for (size_t i = 0; obs_enum_encoder_types(i, &id); i++)
			if (obs_get_encoder_type(id) == OBS_ENCODER_VIDEO)
				printf("ENC %-50s codec=%-6s name=%s\n", id, obs_get_encoder_codec(id),
				       obs_encoder_get_display_name(id));
		obs_shutdown();
		return 0;
	}

	/* Program output: a plain colour. */
	obs_data_t *cs = obs_data_create();
	obs_data_set_int(cs, "color", 0xFF3366CC);
	obs_data_set_int(cs, "width", ovi.base_width);
	obs_data_set_int(cs, "height", ovi.base_height);
	obs_source_t *src = obs_source_create("color_source_v3", "color", cs, NULL);
	obs_data_release(cs);
	obs_set_output_source(0, src);

	/*
	 * One stream per E2E_MEDIA / E2E_MEDIA2 (each through its own filter, all
	 * running at the same time), or one stream of the whole output.
	 */
	const char *medias[2] = {getenv("E2E_MEDIA"), getenv("E2E_MEDIA2")};
	struct stream streams[2] = {0};
	int count = 0;
	for (int i = 0; i < 2; i++) {
		if (i > 0 && !medias[i])
			break;
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s%s", mock, i ? "-2" : "");
		os_mkdirs(dir);
		if (!start_stream(&streams[count], medias[i], token, dir, venc_id))
			return 2;
		count++;
	}

	os_sleep_ms(seconds * 1000);

	for (int i = 0; i < count; i++)
		stop_stream(&streams[i], i);

	obs_set_output_source(0, NULL);
	obs_source_release(src);
	obs_shutdown();
	return 0;
}
