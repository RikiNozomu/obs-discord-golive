/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "discord-dock.hpp"
#include "discord-ids.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#define SETTINGS_FILE "settings.json"

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

DiscordDock::DiscordDock(QWidget *parent) : QWidget(parent)
{
	guildEdit = new QLineEdit(this);
	channelEdit = new QLineEdit(this);
	tokenEdit = new QLineEdit(this);
	tokenBrowse = new QPushButton(T("DiscordGoLive.Browse"), this);
	videoBitrate = new QSpinBox(this);
	audioBitrate = new QSpinBox(this);
	startStop = new QPushButton(T("DiscordGoLive.Start"), this);
	status = new QLabel(this);

	guildEdit->setPlaceholderText(T("DiscordGoLive.IdPlaceholder"));
	channelEdit->setPlaceholderText(T("DiscordGoLive.IdPlaceholder"));
	tokenEdit->setPlaceholderText(T("DiscordGoLive.TokenPlaceholder"));

	videoBitrate->setRange(500, 50000);
	videoBitrate->setSingleStep(500);
	videoBitrate->setSuffix(" Kbps");
	audioBitrate->setRange(32, 256);
	audioBitrate->setSingleStep(32);
	audioBitrate->setSuffix(" Kbps");

	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);

	auto *tokenRow = new QHBoxLayout();
	tokenRow->addWidget(tokenEdit, 1);
	tokenRow->addWidget(tokenBrowse);

	auto *form = new QFormLayout();
	form->addRow(T("DiscordGoLive.GuildId"), guildEdit);
	form->addRow(T("DiscordGoLive.ChannelId"), channelEdit);
	form->addRow(T("DiscordGoLive.TokenFile"), tokenRow);
	form->addRow(T("DiscordGoLive.VideoBitrate"), videoBitrate);
	form->addRow(T("DiscordGoLive.AudioBitrate"), audioBitrate);

	auto *warning = new QLabel(T("DiscordGoLive.TosWarning"), this);
	warning->setWordWrap(true);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(startStop);
	layout->addWidget(status);
	layout->addWidget(warning);
	layout->addStretch();

	connect(startStop, &QPushButton::clicked, this, &DiscordDock::OnStartStopClicked);
	connect(tokenBrowse, &QPushButton::clicked, this, &DiscordDock::OnBrowseToken);

	LoadSettings();
	SetStatus(T("DiscordGoLive.Status.Idle"));
}

DiscordDock::~DiscordDock()
{
	Shutdown();
}

void DiscordDock::Shutdown()
{
	if (output && active)
		obs_output_force_stop(output);
	ReleaseObjects();
}

/* ---------------------------------------------------------------- settings */

void DiscordDock::LoadSettings()
{
	char *path = obs_module_config_path(SETTINGS_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);

	if (!data)
		data = obs_data_create();

	obs_data_set_default_int(data, S_VIDEO_BITRATE, DEFAULT_VIDEO_BITRATE);
	obs_data_set_default_int(data, S_AUDIO_BITRATE, DEFAULT_AUDIO_BITRATE);

	guildEdit->setText(QString::fromUtf8(obs_data_get_string(data, S_GUILD_ID)));
	channelEdit->setText(QString::fromUtf8(obs_data_get_string(data, S_CHANNEL_ID)));
	tokenEdit->setText(QString::fromUtf8(obs_data_get_string(data, S_TOKEN_FILE)));
	videoBitrate->setValue((int)obs_data_get_int(data, S_VIDEO_BITRATE));
	audioBitrate->setValue((int)obs_data_get_int(data, S_AUDIO_BITRATE));

	obs_data_release(data);
}

obs_data_t *DiscordDock::CurrentSettings() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, S_GUILD_ID, guildEdit->text().trimmed().toUtf8().constData());
	obs_data_set_string(data, S_CHANNEL_ID, channelEdit->text().trimmed().toUtf8().constData());
	obs_data_set_string(data, S_TOKEN_FILE, tokenEdit->text().trimmed().toUtf8().constData());
	obs_data_set_int(data, S_VIDEO_BITRATE, videoBitrate->value());
	obs_data_set_int(data, S_AUDIO_BITRATE, audioBitrate->value());
	return data;
}

void DiscordDock::SaveSettings()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	char *path = obs_module_config_path(SETTINGS_FILE);
	if (!path)
		return;

	obs_data_t *data = CurrentSettings();
	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		obs_log(LOG_WARNING, "could not save settings to %s", path);
	obs_data_release(data);
	bfree(path);
}

void DiscordDock::OnBrowseToken()
{
	QString file = QFileDialog::getOpenFileName(this, T("DiscordGoLive.TokenFile"), tokenEdit->text());
	if (!file.isEmpty())
		tokenEdit->setText(file);
}

/* ------------------------------------------------------------- start/stop */

void DiscordDock::OnStartStopClicked()
{
	if (active) {
		StopStream();
		return;
	}

	SaveSettings();
	if (!StartStream())
		ReleaseObjects();
}

bool DiscordDock::StartStream()
{
	ReleaseObjects();

	obs_data_t *settings = CurrentSettings();
	service = obs_service_create_private(DISCORD_SERVICE_ID, "discord golive service", settings);
	obs_data_release(settings);

	if (!service || !obs_service_can_try_to_connect(service)) {
		SetStatus(T("DiscordGoLive.Error.Settings"));
		return false;
	}

	obs_data_t *vset = obs_data_create();
	obs_data_t *aset = obs_data_create();
	obs_data_set_int(vset, "bitrate", videoBitrate->value());
	obs_data_set_int(vset, "keyint_sec", DEFAULT_KEYINT_SEC);
	obs_data_set_string(vset, "preset", "veryfast");
	obs_data_set_int(aset, "bitrate", audioBitrate->value());
	obs_service_apply_encoder_settings(service, vset, aset);

	/* M4 lets the user choose a hardware H.264 encoder; x264 exists on every platform. */
	videoEncoder = obs_video_encoder_create("obs_x264", "discord golive video", vset, nullptr);
	audioEncoder = obs_audio_encoder_create("ffmpeg_opus", "discord golive audio", aset, 0, nullptr);
	obs_data_release(vset);
	obs_data_release(aset);

	if (!videoEncoder || !audioEncoder) {
		SetStatus(T("DiscordGoLive.Error.Encoders"));
		return false;
	}

	obs_encoder_set_video(videoEncoder, obs_get_video());
	obs_encoder_set_audio(audioEncoder, obs_get_audio());

	output = obs_output_create(DISCORD_OUTPUT_ID, "discord golive output", nullptr, nullptr);
	if (!output) {
		SetStatus(T("DiscordGoLive.Error.Output"));
		return false;
	}

	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);
	obs_output_set_service(output, service);
	obs_output_set_reconnect_settings(output, 10, 5);

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", OutputStartedCb, this);
	signal_handler_connect(sh, "stop", OutputStoppedCb, this);

	SetEditable(false);
	SetStatus(T("DiscordGoLive.Status.Starting"));

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		SetStatus(T("DiscordGoLive.Status.Failed")
				  .arg(err ? QString::fromUtf8(err) : T("DiscordGoLive.Error.Unknown")));
		SetEditable(true);
		return false;
	}

	return true;
}

void DiscordDock::StopStream()
{
	if (output) {
		SetStatus(T("DiscordGoLive.Status.Stopping"));
		startStop->setEnabled(false);
		obs_output_stop(output);
	}
}

void DiscordDock::ReleaseObjects()
{
	if (output) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_disconnect(sh, "start", OutputStartedCb, this);
		signal_handler_disconnect(sh, "stop", OutputStoppedCb, this);
	}

	obs_output_release(output);
	obs_encoder_release(videoEncoder);
	obs_encoder_release(audioEncoder);
	obs_service_release(service);

	output = nullptr;
	videoEncoder = nullptr;
	audioEncoder = nullptr;
	service = nullptr;
	active = false;
}

/* Output signals arrive on OBS threads; hop to the UI thread. */
void DiscordDock::OutputStartedCb(void *data, calldata_t *)
{
	auto *dock = static_cast<DiscordDock *>(data);
	QMetaObject::invokeMethod(dock, "OnOutputStarted", Qt::QueuedConnection);
}

void DiscordDock::OutputStoppedCb(void *data, calldata_t *cd)
{
	auto *dock = static_cast<DiscordDock *>(data);
	int code = (int)calldata_int(cd, "code");
	obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *err = out ? obs_output_get_last_error(out) : nullptr;
	QString error = err ? QString::fromUtf8(err) : QString();

	QMetaObject::invokeMethod(dock, "OnOutputStopped", Qt::QueuedConnection, Q_ARG(int, code),
				  Q_ARG(QString, error));
}

void DiscordDock::OnOutputStarted()
{
	active = true;
	startStop->setText(T("DiscordGoLive.Stop"));
	startStop->setEnabled(true);
	SetStatus(T("DiscordGoLive.Status.Live"));
}

void DiscordDock::OnOutputStopped(int code, const QString &error)
{
	active = false;
	startStop->setText(T("DiscordGoLive.Start"));
	startStop->setEnabled(true);
	SetEditable(true);

	if (code == OBS_OUTPUT_SUCCESS)
		SetStatus(T("DiscordGoLive.Status.Idle"));
	else
		SetStatus(T("DiscordGoLive.Status.Failed")
				  .arg(error.isEmpty() ? T("DiscordGoLive.Error.Unknown") : error));

	ReleaseObjects();
}

void DiscordDock::SetEditable(bool editable)
{
	guildEdit->setEnabled(editable);
	channelEdit->setEnabled(editable);
	tokenEdit->setEnabled(editable);
	tokenBrowse->setEnabled(editable);
	videoBitrate->setEnabled(editable);
	audioBitrate->setEnabled(editable);
}

void DiscordDock::SetStatus(const QString &text)
{
	status->setText(text);
}

/* ------------------------------------------------------------ registration */

static DiscordDock *g_dock = nullptr;

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && g_dock)
		g_dock->Shutdown();
}

extern "C" void discord_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	g_dock = new DiscordDock(main);

	if (!obs_frontend_add_dock_by_id("obs-discord-golive-dock", obs_module_text("DiscordGoLive.DockTitle"), g_dock))
		obs_log(LOG_WARNING, "could not add dock");

	obs_frontend_add_event_callback(frontend_event, nullptr);
}

extern "C" void discord_dock_unregister(void)
{
	if (!g_dock)
		return;

	obs_frontend_remove_event_callback(frontend_event, nullptr);
	g_dock = nullptr; /* Owned and deleted by the OBS main window. */
}
