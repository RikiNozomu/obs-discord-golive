/* Headless libobs test for obs-discord-golive. Run via tests/run-module-test.sh. */
#include <obs.h>
#include <stdio.h>
#include <string.h>
static int fails = 0;
#define CHECK(c, msg) do { bool ok_ = (c); printf("%s %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) fails++; } while (0)
static bool has_type(bool (*en)(size_t, const char **), const char *id) {
	const char *t; for (size_t i = 0; en(i, &t); i++) if (!strcmp(t, id)) return true; return false;
}
int main(int argc, char **argv) {
	const char *so = argv[1], *data = argv[2], *tokfile = argv[3];
	if (!obs_startup("en-US", NULL, NULL)) return 1;
	obs_module_t *mod = NULL;
	CHECK(obs_open_module(&mod, so, data) == MODULE_SUCCESS, "module opens");
	CHECK(obs_init_module(mod), "obs_module_load succeeds");
	CHECK(has_type(obs_enum_service_types, "discord_golive_service"), "service type registered");
	CHECK(has_type(obs_enum_output_types, "discord_golive_output"), "output type registered");
	printf("     service display name: %s\n", obs_service_get_display_name("discord_golive_service"));

	obs_data_t *s = obs_data_create();
	obs_service_t *svc = obs_service_create_private("discord_golive_service", "t", s);
	CHECK(!obs_service_can_try_to_connect(svc), "empty settings rejected");
	obs_data_set_string(s, "guild_id", "123456789012345678");
	obs_data_set_string(s, "channel_id", "abc");
	obs_data_set_string(s, "token_file", "/nonexistent/token");
	obs_service_update(svc, s);
	CHECK(!obs_service_can_try_to_connect(svc), "non-numeric channel ID rejected");
	obs_data_set_string(s, "channel_id", "987654321098765432");
	obs_service_update(svc, s);
	CHECK(obs_service_can_try_to_connect(svc), "valid IDs accepted");
	CHECK(!strcmp(obs_service_get_protocol(svc), "DISCORD"), "protocol is DISCORD");
	CHECK(!strcmp(obs_service_get_preferred_output_type(svc), "discord_golive_output"), "preferred output type");
	const char **vc = obs_service_get_supported_video_codecs(svc), **ac = obs_service_get_supported_audio_codecs(svc);
	CHECK(vc && !strcmp(vc[0], "h264") && !vc[1], "video codecs = h264");
	CHECK(ac && !strcmp(ac[0], "opus") && !ac[1], "audio codecs = opus");

	obs_data_t *v = obs_data_create(), *a = obs_data_create();
	obs_data_set_int(v, "bf", 2); obs_data_set_int(v, "keyint_sec", 10); obs_data_set_int(a, "bitrate", 999);
	obs_service_apply_encoder_settings(svc, v, a);
	CHECK(obs_data_get_int(v, "bf") == 0, "B-frames forced to 0");
	CHECK(obs_data_get_int(v, "keyint_sec") == 2, "keyframe interval clamped to 2 s");
	CHECK(!strcmp(obs_data_get_string(v, "rate_control"), "CBR"), "CBR forced");
	CHECK(obs_data_get_int(a, "bitrate") == 128, "bad audio bitrate reset to 128");

	obs_output_t *out = obs_output_create("discord_golive_output", "t", NULL, NULL);
	CHECK(out != NULL, "output created");
	obs_output_set_service(out, svc);
	CHECK(obs_output_get_service(out) == svc, "service attached to output");
	CHECK(!obs_output_start(out), "start fails with missing token file");
	printf("     last error: %s\n", obs_output_get_last_error(out));
	CHECK(obs_output_get_last_error(out) && strstr(obs_output_get_last_error(out), "token file"), "error names token file");

	obs_data_set_string(s, "token_file", tokfile);
	obs_service_update(svc, s);
	CHECK(!obs_output_start(out), "start fails without encoders");
	printf("     last error: %s\n", obs_output_get_last_error(out));
	CHECK(strstr(obs_output_get_last_error(out), "encoders") != NULL, "error names encoders");

	obs_output_release(out); obs_service_release(svc);
	obs_data_release(s); obs_data_release(v); obs_data_release(a);
	obs_shutdown();
	printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
	return fails != 0;
}
