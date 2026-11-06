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

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/*
 * "Discord Go Live" dock: holds the settings, owns the private service,
 * encoders and output, and offers Start/Stop. It runs alongside OBS's
 * main stream and recording.
 */
class DiscordDock : public QWidget {
	Q_OBJECT

public:
	explicit DiscordDock(QWidget *parent = nullptr);
	~DiscordDock() override;

	/* Stops the stream and frees OBS objects. Safe to call twice. */
	void Shutdown();

private slots:
	void OnStartStopClicked();
	void OnBrowseToken();
	void OnOutputStarted();
	void OnOutputStopped(int code, const QString &error);

private:
	void LoadSettings();
	void SaveSettings();
	obs_data_t *CurrentSettings() const;
	bool StartStream();
	void StopStream();
	void ReleaseObjects();
	void SetEditable(bool editable);
	void SetStatus(const QString &text);

	static void OutputStartedCb(void *data, calldata_t *cd);
	static void OutputStoppedCb(void *data, calldata_t *cd);

	QLineEdit *guildEdit;
	QLineEdit *channelEdit;
	QLineEdit *tokenEdit;
	QPushButton *tokenBrowse;
	QSpinBox *videoBitrate;
	QSpinBox *audioBitrate;
	QPushButton *startStop;
	QLabel *status;

	obs_service_t *service = nullptr;
	obs_encoder_t *videoEncoder = nullptr;
	obs_encoder_t *audioEncoder = nullptr;
	obs_output_t *output = nullptr;
	bool active = false;
};
