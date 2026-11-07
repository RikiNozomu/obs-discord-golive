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

#include <QProcess>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

/*
 * "Discord Go Live" dock. The flow is:
 *
 *   1. Connect: the helper logs in once (--list) and reports the voice
 *      channels the account may stream to, and any Go Live it already runs.
 *   2. Choose a voice channel, then Start: the dock creates a private
 *      service, encoders and output (the output runs the helper again, this
 *      time to stream). It runs alongside OBS's main stream and recording.
 *
 * If the account is already streaming somewhere else, the Start button turns
 * into "Stop streaming in ..." (helper --end-stream).
 */
class DiscordDock : public QWidget {
	Q_OBJECT

public:
	explicit DiscordDock(QWidget *parent = nullptr);
	~DiscordDock() override;

	/* Stops the stream and frees OBS objects. Safe to call twice. */
	void Shutdown();

private slots:
	void OnConnectClicked();
	void OnStartStopClicked();
	void OnAccountFinished(int exitCode, QProcess::ExitStatus exitStatus);
	void OnAccountTimeout();
	void OnOutputStarted();
	void OnOutputStopped(int code, const QString &error);

private:
	enum class Job { None, List, EndStream };

	struct Channel {
		QString guildId, channelId, label;
		bool valid() const { return !guildId.isEmpty(); }
	};

	void LoadSettings();
	void SaveSettings();
	obs_data_t *CurrentSettings() const;
	QString SaveToken();
	bool StartStream();
	void StopStream();
	void ReleaseObjects();

	void RunAccountJob(Job job);
	void StopAccountJob();
	void HandleListResult(const QJsonObject &result);
	void Disconnect();
	Channel SelectedChannel() const;
	void UpdateUi();
	void SetStatus(const QString &text);

	static void OutputStartedCb(void *data, calldata_t *cd);
	static void OutputStoppedCb(void *data, calldata_t *cd);

	QLineEdit *tokenEdit;
	QCheckBox *tokenShow;
	QPushButton *connectButton;
	QPushButton *refreshButton;
	QComboBox *channelCombo;
	QComboBox *qualityCombo;
	QPushButton *startStop;
	QLabel *status;

	/* Account helper run (Connect / stop a stream elsewhere). */
	QProcess *account = nullptr;
	QTimer *accountTimer;
	Job job = Job::None;
	bool connected = false;
	Channel remoteStream; /* The account's Go Live running elsewhere, if any. */

	/* Last chosen channel, to preselect it after Connect. */
	QString savedGuild;
	QString savedChannel;

	obs_service_t *service = nullptr;
	obs_encoder_t *videoEncoder = nullptr;
	obs_encoder_t *audioEncoder = nullptr;
	obs_output_t *output = nullptr; /* Non-null from Start until the output stops. */
	bool active = false;            /* The output reported "start" (live). */
	bool stopping = false;
};
