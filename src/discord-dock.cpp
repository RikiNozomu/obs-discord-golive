/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "discord-dock.hpp"
#include "discord-filter.h"
#include "discord-ids.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QThread>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#define SETTINGS_FILE "settings.json"
/* The token lives in its own owner-only file, never in settings.json (GUIDELINE.md §6). */
#define TOKEN_FILE "token"
/* The helper's own login timeout is 20 s; this only catches a hung helper. */
#define ACCOUNT_TIMEOUT_MS 45000

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/*
 * Test mode, for developers only (no UI): set OBS_DISCORD_GOLIVE_MOCK_DIR to a
 * folder before starting OBS, and the helper dumps the stream there and uses
 * fake channels instead of contacting Discord.
 */
static QString MockDir()
{
	return qEnvironmentVariable("OBS_DISCORD_GOLIVE_MOCK_DIR").trimmed();
}

/*
 * Resolution and frame rate of the Discord stream only; OBS's own canvas and
 * other outputs are untouched. Without Nitro, Go Live allows 720p at 30 fps.
 */
struct StreamQuality {
	const char *label; /* locale key */
	uint32_t height;   /* 0 = same as OBS */
	uint32_t fps;      /* 0 = same as OBS */
};

static const StreamQuality kQualities[] = {
	{"DiscordGoLive.Quality.720p30", 720, 30},
	{"DiscordGoLive.Quality.1080p30", 1080, 30},
	{"DiscordGoLive.Quality.1080p60", 1080, 60},
	{"DiscordGoLive.Quality.Source", 0, 0},
};

/* What the encoder will produce for a quality choice. */
struct StreamPlan {
	uint32_t width = 0, height = 0; /* size the encoder produces */
	bool scale = false;
	uint32_t divisor = 1;           /* frame rate divisor */
	double fps = 0;
	int videoKbps = DEFAULT_VIDEO_BITRATE;
};

/*
 * Never upscales or adds frames. The bitrate follows the real output size:
 * about 0.1 bit per pixel per frame, rounded to 500 Kbps and kept within
 * 2500..MAX_VIDEO_BITRATE (720p30 -> 3000, 1080p30 -> 6000, 1080p60 -> 8000).
 */
static StreamPlan PlanQuality(int index, uint32_t sourceWidth = 0, uint32_t sourceHeight = 0)
{
	StreamPlan plan;
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.output_height || !ovi.fps_den)
		return plan;
	if (index < 0 || index >= (int)(sizeof(kQualities) / sizeof(kQualities[0])))
		index = 0;
	const StreamQuality &q = kQualities[index];

	/* A single source streams at its own size; the whole output at OBS's output size. */
	uint32_t w = sourceWidth ? sourceWidth : ovi.output_width;
	uint32_t h = sourceHeight ? sourceHeight : ovi.output_height;
	if (q.height && h > q.height) {
		w = (uint32_t)((double)w * q.height / h + 0.5) & ~1u;
		h = q.height;
		plan.scale = true;
	}
	plan.width = w;
	plan.height = h;

	double obsFps = (double)ovi.fps_num / ovi.fps_den;
	if (q.fps) {
		uint32_t divisor = (uint32_t)(obsFps / q.fps + 0.5);
		if (divisor > 1)
			plan.divisor = divisor;
	}
	plan.fps = obsFps / plan.divisor;

	double kbps = (double)w * h * plan.fps * 0.1 / 1000.0;
	int rounded = (int)(kbps / 500.0 + 0.5) * 500;
	plan.videoKbps = rounded < 2500 ? 2500 : rounded > MAX_VIDEO_BITRATE ? MAX_VIDEO_BITRATE : rounded;
	return plan;
}

/*
 * Video encoders, best first. A hardware encoder takes the work off the CPU,
 * which keeps OBS from skipping frames under load (each skip is a hiccup
 * viewers see). x264 exists everywhere and is the fallback.
 */
static const char *const kHardwareEncoders[] = {
	"com.apple.videotoolbox.videoencoder.ave.avc", /* macOS: Apple VT H264 Hardware Encoder */
	"obs_nvenc_h264_tex",                          /* NVIDIA NVENC */
};
static const char *const kSoftwareEncoder = "obs_x264";

static bool EncoderRegistered(const char *id)
{
	const char *type;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (strcmp(type, id) == 0)
			return true;
	}
	return false;
}

static const char *PickVideoEncoder(bool allowHardware)
{
	if (allowHardware) {
		for (const char *id : kHardwareEncoders) {
			if (EncoderRegistered(id))
				return id;
		}
	}
	return kSoftwareEncoder;
}

/*
 * Per-encoder settings on top of the service's (x264-style) ones: no
 * B-frames, constant-ish bitrate, low latency. Keys come from each encoder in
 * OBS 31 (mac-videotoolbox, obs-nvenc).
 */
static void ApplyEncoderSettings(const char *id, obs_data_t *video, int kbps)
{
	if (strstr(id, "videotoolbox")) {
		/* CBR exists only on Apple Silicon with macOS 13+; capped ABR works on every Mac. */
		obs_data_set_string(video, "rate_control", "ABR");
		obs_data_set_bool(video, "limit_bitrate", true);
		obs_data_set_int(video, "max_bitrate", kbps);
		obs_data_set_double(video, "max_bitrate_window", 1.0);
		obs_data_set_bool(video, "bframes", false);
		obs_data_set_string(video, "profile", "main");
	} else if (strstr(id, "nvenc")) {
		obs_data_set_string(video, "rate_control", "CBR");
		obs_data_set_string(video, "preset", "p4");
		obs_data_set_string(video, "tune", "ll");
		obs_data_set_string(video, "multipass", "disabled");
		obs_data_set_int(video, "bf", 0);
		obs_data_set_bool(video, "lookahead", false);
		obs_data_set_string(video, "profile", "main");
	}
}

DiscordDock::DiscordDock(QWidget *parent, obs_source_t *filter) : QWidget(parent)
{
	if (filter) {
		filterWeak = obs_source_get_weak_source(filter);
		filterPtr = filter;
		filterUuid = QString::fromUtf8(obs_source_get_uuid(filter));
		/*
		 * Deleting the filter fires "filter_remove" on its source; deleting
		 * the source fires "remove". Either one stops this window's stream.
		 */
		if (obs_source_t *parent = obs_filter_get_parent(filter)) {
			parentWeak = obs_source_get_weak_source(parent);
			signal_handler_t *sh = obs_source_get_signal_handler(parent);
			signal_handler_connect(sh, "filter_remove", FilterRemovedCb, this);
			signal_handler_connect(sh, "remove", SourceRemovedCb, this);
		}
	}

	tokenEdit = new QLineEdit(this);
	tokenShow = new QCheckBox(T("DiscordGoLive.ShowToken"), this);
	connectButton = new QPushButton(T("DiscordGoLive.Connect"), this);
	refreshButton = new QPushButton(T("DiscordGoLive.Refresh"), this);
	channelCombo = new QComboBox(this);
	encoderCombo = new QComboBox(this);
	encoderCombo->addItem(T("DiscordGoLive.Encoder.Auto"));
	encoderCombo->addItem(T("DiscordGoLive.Encoder.Software"));
	qualityCombo = new QComboBox(this);
	for (const StreamQuality &q : kQualities)
		qualityCombo->addItem(T(q.label));
	startStop = new QPushButton(T("DiscordGoLive.Start"), this);
	status = new QLabel(this);
	accountTimer = new QTimer(this);

	tokenEdit->setPlaceholderText(T("DiscordGoLive.TokenPlaceholder"));
	tokenEdit->setEchoMode(QLineEdit::Password);
	channelCombo->setPlaceholderText(T("DiscordGoLive.ConnectFirst"));
	channelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	channelCombo->setMinimumContentsLength(12);
	channelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);


	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);

	accountTimer->setSingleShot(true);
	accountTimer->setInterval(ACCOUNT_TIMEOUT_MS);

	auto *tokenRow = new QHBoxLayout();
	tokenRow->addWidget(tokenEdit, 1);
	tokenRow->addWidget(tokenShow);

	auto *form = new QFormLayout();
	form->addRow(T("DiscordGoLive.Token"), tokenRow);
	auto *connectRow = new QHBoxLayout();
	connectRow->addWidget(connectButton);
	connectRow->addWidget(refreshButton);
	connectRow->addStretch();
	form->addRow(QString(), connectRow);
	form->addRow(T("DiscordGoLive.VoiceChannel"), channelCombo);
	form->addRow(T("DiscordGoLive.Quality"), qualityCombo);
	form->addRow(T("DiscordGoLive.Encoder"), encoderCombo);

	auto *warning = new QLabel(T("DiscordGoLive.TosWarning"), this);
	warning->setWordWrap(true);

	auto *version = new QLabel(T("DiscordGoLive.Version").arg(PLUGIN_VERSION, PLUGIN_BUILD_NUMBER), this);
	version->setEnabled(false); /* greyed out, like a footnote */

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(startStop);
	layout->addWidget(status);
	layout->addWidget(warning);
	layout->addWidget(version);
	layout->addStretch();

	connect(connectButton, &QPushButton::clicked, this, &DiscordDock::OnConnectClicked);
	connect(refreshButton, &QPushButton::clicked, this, [this]() { RunAccountJob(Job::List); });
	connect(startStop, &QPushButton::clicked, this, &DiscordDock::OnStartStopClicked);
	connect(tokenShow, &QCheckBox::toggled, this, [this](bool show) {
		tokenEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
	});
	connect(accountTimer, &QTimer::timeout, this, &DiscordDock::OnAccountTimeout);
	connect(channelCombo, &QComboBox::currentIndexChanged, this, [this]() {
		Channel c = SelectedChannel();
		if (c.valid()) {
			savedGuild = c.guildId;
			savedChannel = c.channelId;
		}
		UpdateUi();
	});

	LoadSettings();

	/* A different account or mode makes the channel list stale. */
	connect(tokenEdit, &QLineEdit::textChanged, this, &DiscordDock::Disconnect);

	SetStatus(T("DiscordGoLive.Status.NotConnected"));
	UpdateUi();
}

DiscordDock::~DiscordDock()
{
	Shutdown();
}

void DiscordDock::Shutdown()
{
	StopAccountJob();
	if (output && active)
		obs_output_force_stop(output);
	ReleaseObjects();

	if (parentWeak) {
		if (obs_source_t *parent = obs_weak_source_get_source(parentWeak)) {
			signal_handler_t *sh = obs_source_get_signal_handler(parent);
			signal_handler_disconnect(sh, "filter_remove", FilterRemovedCb, this);
			signal_handler_disconnect(sh, "remove", SourceRemovedCb, this);
			obs_source_release(parent);
		}
		obs_weak_source_release(parentWeak);
		parentWeak = nullptr;
	}
	if (filterWeak) {
		obs_weak_source_release(filterWeak);
		filterWeak = nullptr;
	}
}

/* ---------------------------------------------------------------- settings */

void DiscordDock::LoadSettings()
{
	obs_data_t *data = nullptr;
	if (obs_source_t *filter = Filter()) {
		/* A filter's window keeps its settings in the filter (saved with the scene collection). */
		data = obs_source_get_settings(filter);
		obs_source_release(filter);
	} else {
		char *path = obs_module_config_path(SETTINGS_FILE);
		data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
		bfree(path);
	}

	if (!data)
		data = obs_data_create();


	savedGuild = QString::fromUtf8(obs_data_get_string(data, S_GUILD_ID));
	savedChannel = QString::fromUtf8(obs_data_get_string(data, S_CHANNEL_ID));
	QFile tokenFile(TokenPath());
	if (tokenFile.open(QIODevice::ReadOnly))
		tokenEdit->setText(QString::fromUtf8(tokenFile.readAll()).trimmed());
	qualityCombo->setCurrentIndex((int)obs_data_get_int(data, S_QUALITY));
	encoderCombo->setCurrentIndex((int)obs_data_get_int(data, S_ENCODER));

	obs_data_release(data);
}

obs_data_t *DiscordDock::CurrentSettings() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, S_GUILD_ID, savedGuild.toUtf8().constData());
	obs_data_set_string(data, S_CHANNEL_ID, savedChannel.toUtf8().constData());
	if (!savedGuild.isEmpty())
		obs_data_set_string(data, S_CHANNEL, QString("%1:%2").arg(savedGuild, savedChannel).toUtf8().constData());
	obs_data_set_int(data, S_QUALITY, qualityCombo->currentIndex());
	obs_data_set_int(data, S_ENCODER, encoderCombo->currentIndex());
	return data;
}

void DiscordDock::SaveSettings()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = CurrentSettings();
	if (obs_source_t *filter = Filter()) {
		obs_source_update(filter, data);
		obs_source_release(filter);
	} else if (char *path = obs_module_config_path(SETTINGS_FILE)) {
		if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
			obs_log(LOG_WARNING, "could not save settings to %s", path);
		bfree(path);
	}
	obs_data_release(data);
}

/* The filter this window streams, with a new reference; nullptr for the dock or a removed filter. */
obs_source_t *DiscordDock::Filter() const
{
	return filterWeak ? obs_weak_source_get_source(filterWeak) : nullptr;
}

/*
 * The dock's token is <plugin config>/token; each filter window has its own
 * account, in <plugin config>/tokens/<filter uuid>. Never in OBS's settings.
 */
QString DiscordDock::TokenPath() const
{
	QString name = filterUuid.isEmpty() ? QString(TOKEN_FILE) : QString("tokens/%1").arg(filterUuid);
	char *raw = obs_module_config_path(name.toUtf8().constData());
	QString path = raw ? QString::fromUtf8(raw) : QString();
	bfree(raw);
	return path;
}

/*
 * Writes the token to <plugin config>/token, readable only by the current
 * user, and returns that path; returns an empty string if there is no token.
 * The helper reads the file, so the token never appears in argv or logs.
 */
QString DiscordDock::SaveToken()
{
	QString path = TokenPath();
	if (path.isEmpty())
		return QString();
	os_mkdirs(QFileInfo(path).absolutePath().toUtf8().constData());

	QByteArray token = tokenEdit->text().trimmed().toUtf8();
	if (token.isEmpty()) {
		QFile::remove(path);
		return QString();
	}

	const QFileDevice::Permissions ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
	QFile file(path);
	/* Create it owner-only from the start; also tighten an existing file. */
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate, ownerOnly) || !file.setPermissions(ownerOnly) ||
	    file.write(token) != token.size() || !file.flush()) {
		obs_log(LOG_WARNING, "could not save the token file");
		return QString();
	}
	return path;
}

/* ---------------------------------------------------------------- account */

/* While a helper run is in progress the button reads "Disconnect" and cancels it. */
/* Connect when disconnected; otherwise Disconnect (also cancels a run in progress). */
void DiscordDock::OnConnectClicked()
{
	if (account || connected) {
		StopAccountJob();
		Disconnect();
		SetStatus(T("DiscordGoLive.Status.Disconnected"));
		return;
	}
	RunAccountJob(Job::List);
}

/* Kills a running helper (--list / --end-stream) without reading its answer. */
void DiscordDock::StopAccountJob()
{
	if (!account)
		return;
	accountTimer->stop();
	account->disconnect(this);
	account->kill();
	account->waitForFinished(1000);
	account->deleteLater();
	account = nullptr;
	job = Job::None;
}

/* Runs the helper once (--list or --end-stream); OnAccountFinished reads the answer. */
void DiscordDock::RunAccountJob(Job newJob)
{
	if (account || output)
		return;

	SaveSettings();
	QString tokenPath = SaveToken();
	if (tokenPath.isEmpty()) {
		SetStatus(T("DiscordGoLive.Error.NoToken"));
		return;
	}

	char *argv[4] = {};
	size_t argc = 0;
	const char *error = discord_resolve_helper(nullptr, argv, &argc);
	if (error) {
		SetStatus(T(error));
		return;
	}

	QString program = QString::fromUtf8(argv[0]);
	QStringList args;
	for (size_t i = 1; i < argc; i++)
		args << QString::fromUtf8(argv[i]);
	for (size_t i = 0; i < argc; i++)
		bfree(argv[i]);

	if (newJob == Job::List)
		args << "--list";
	else
		args << "--end-stream" << remoteStream.guildId << remoteStream.channelId;

	QString mockDir = MockDir();
	if (!mockDir.isEmpty())
		args << "--mock" << mockDir;
	else
		args << "--token-file" << tokenPath; /* A path, never the token itself. */

	job = newJob;
	account = new QProcess(this);
	account->setProgram(program);
	account->setArguments(args);
	account->setProcessChannelMode(QProcess::SeparateChannels);
	connect(account, &QProcess::finished, this, &DiscordDock::OnAccountFinished);
	connect(account, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
		if (err == QProcess::FailedToStart)
			OnAccountFinished(-1, QProcess::CrashExit);
	});
	account->start();
	account->closeWriteChannel();
	accountTimer->start();

	SetStatus(newJob == Job::List ? T("DiscordGoLive.Status.Connecting")
				      : T("DiscordGoLive.Status.EndingRemote").arg(remoteStream.label));
	UpdateUi();
}

void DiscordDock::OnAccountTimeout()
{
	if (!account)
		return;
	StopAccountJob();
	SetStatus(T("DiscordGoLive.Error.Connect").arg(T("DiscordGoLive.Error.ConnectTimeout")));
	UpdateUi();
}

void DiscordDock::OnAccountFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
	if (!account)
		return;
	accountTimer->stop();

	/* stderr carries LOG / ERROR / RESULT lines (helper/src/ipc.ts); the helper redacts tokens. */
	QString errorText;
	QJsonObject result;
	bool haveResult = false;
	const QList<QByteArray> lines = account->readAllStandardError().split('\n');
	for (const QByteArray &raw : lines) {
		QString line = QString::fromUtf8(raw).trimmed();
		if (line.startsWith("RESULT ")) {
			QJsonDocument doc = QJsonDocument::fromJson(line.mid(7).toUtf8());
			haveResult = doc.isObject();
			result = doc.object();
		} else if (line.startsWith("ERROR ")) {
			errorText = line.mid(6);
		} else if (line.startsWith("LOG ")) {
			obs_log(LOG_INFO, "[discord-helper] %s", line.mid(4).toUtf8().constData());
		}
	}

	Job finished = job;
	account->disconnect(this);
	account->deleteLater();
	account = nullptr;
	job = Job::None;

	if (exitStatus != QProcess::NormalExit || exitCode != 0 || !haveResult) {
		if (errorText.isEmpty())
			errorText = exitCode == -1 ? T("DiscordGoLive.Error.HelperStart")
						   : T("DiscordGoLive.Error.HelperExited");
		if (finished == Job::List)
			Disconnect();
		SetStatus(T("DiscordGoLive.Error.Connect").arg(errorText));
		UpdateUi();
		return;
	}

	if (finished == Job::List) {
		HandleListResult(result);
		return;
	}

	/* --end-stream: refresh the list so the button reflects what Discord now says. */
	if (result.value("ended").toBool()) {
		obs_log(LOG_INFO, "stopped the account's stream elsewhere");
		RunAccountJob(Job::List);
	} else {
		SetStatus(T("DiscordGoLive.Error.RemoteNotEnded").arg(remoteStream.label));
		UpdateUi();
	}
}

static QString ChannelLabel(const QJsonObject &c)
{
	return QString("%1 › %2").arg(c.value("guildName").toString(), c.value("channelName").toString());
}

void DiscordDock::HandleListResult(const QJsonObject &result)
{
	const QSignalBlocker block(channelCombo);
	channelCombo->clear();

	int select = -1;
	const QJsonArray channels = result.value("channels").toArray();
	for (const QJsonValue v : channels) {
		QJsonObject c = v.toObject();
		QString guild = c.value("guildId").toString();
		QString channel = c.value("channelId").toString();
		channelCombo->addItem(ChannelLabel(c), QStringList{guild, channel});
		if (guild == savedGuild && channel == savedChannel)
			select = channelCombo->count() - 1;
	}
	channelCombo->setPlaceholderText(T("DiscordGoLive.ChooseChannel"));
	channelCombo->setCurrentIndex(select);

	remoteStream = Channel();
	QJsonObject remote = result.value("activeStream").toObject();
	if (!remote.isEmpty())
		remoteStream = {remote.value("guildId").toString(), remote.value("channelId").toString(),
				ChannelLabel(remote)};

	connected = true;
	QString user = result.value("user").toString();
	if (remoteStream.valid())
		SetStatus(T("DiscordGoLive.Status.RemoteStream").arg(user, remoteStream.label));
	else if (channels.isEmpty())
		SetStatus(T("DiscordGoLive.Status.NoChannels").arg(user));
	else
		SetStatus(T("DiscordGoLive.Status.Connected").arg(user));
	UpdateUi();
}

void DiscordDock::Disconnect()
{
	if (connected || remoteStream.valid()) {
		connected = false;
		remoteStream = Channel();
		const QSignalBlocker block(channelCombo);
		channelCombo->clear();
		channelCombo->setPlaceholderText(T("DiscordGoLive.ConnectFirst"));
		SetStatus(T("DiscordGoLive.Status.NotConnected"));
	}
	UpdateUi();
}

DiscordDock::Channel DiscordDock::SelectedChannel() const
{
	QStringList ids = channelCombo->currentData().toStringList();
	if (ids.size() != 2)
		return Channel();
	return {ids[0], ids[1], channelCombo->currentText()};
}

/* ------------------------------------------------------------- start/stop */

void DiscordDock::OnStartStopClicked()
{
	if (output) {
		StopStream();
		return;
	}
	if (remoteStream.valid()) {
		RunAccountJob(Job::EndStream);
		return;
	}

	SaveSettings();
	bool allowHardware = encoderCombo->currentIndex() == 0;
	bool started = StartStream(allowHardware);
	if (!started && usedHardware) {
		/* The hardware encoder refused to start (busy, unsupported size...): use x264. */
		obs_log(LOG_WARNING, "hardware video encoder failed to start, retrying with x264");
		ReleaseObjects();
		started = StartStream(false);
	}
	if (!started)
		ReleaseObjects();
	UpdateUi();
}

bool DiscordDock::StartStream(bool allowHardware)
{
	ReleaseObjects();

	Channel channel = SelectedChannel();
	if (!connected || !channel.valid()) {
		SetStatus(T("DiscordGoLive.Error.Settings"));
		return false;
	}

	obs_data_t *settings = CurrentSettings();
	obs_data_set_string(settings, S_GUILD_ID, channel.guildId.toUtf8().constData());
	obs_data_set_string(settings, S_CHANNEL_ID, channel.channelId.toUtf8().constData());
	obs_data_set_string(settings, S_TOKEN_FILE, SaveToken().toUtf8().constData());
	obs_data_set_string(settings, S_MOCK_DIR, MockDir().toUtf8().constData());
	service = obs_service_create_private(DISCORD_SERVICE_ID, "discord golive service", settings);
	obs_data_release(settings);

	if (!service || !obs_service_can_try_to_connect(service)) {
		SetStatus(T("DiscordGoLive.Error.Settings"));
		return false;
	}

	/* A filter's window streams only that source: its own picture and sound. */
	video_t *video = obs_get_video();
	audio_t *audio = obs_get_audio();
	uint32_t sourceWidth = 0, sourceHeight = 0;
	if (filterWeak) {
		streamFilter = Filter();
		if (!streamFilter) {
			SetStatus(T("DiscordGoLive.Error.FilterMissing"));
			return false;
		}
		const char *error = discord_filter_begin(streamFilter, &video, &audio, &sourceWidth, &sourceHeight);
		if (error) {
			SetStatus(T(error));
			return false;
		}
	}

	const StreamPlan plan = PlanQuality(qualityCombo->currentIndex(), sourceWidth, sourceHeight);
	obs_log(LOG_INFO, "stream quality: %ux%u at %.2f fps, %d Kbps video, %d Kbps audio",
		plan.width, plan.height, plan.fps, plan.videoKbps, DEFAULT_AUDIO_BITRATE);

	obs_data_t *vset = obs_data_create();
	obs_data_t *aset = obs_data_create();
	obs_data_set_int(vset, "bitrate", plan.videoKbps);
	obs_data_set_int(vset, "keyint_sec", DEFAULT_KEYINT_SEC);
	obs_data_set_string(vset, "preset", "veryfast");
	obs_data_set_int(aset, "bitrate", DEFAULT_AUDIO_BITRATE);
	obs_service_apply_encoder_settings(service, vset, aset);

	const char *encoderId = PickVideoEncoder(allowHardware);
	usedHardware = strcmp(encoderId, kSoftwareEncoder) != 0;
	ApplyEncoderSettings(encoderId, vset, plan.videoKbps);
	obs_log(LOG_INFO, "video encoder: %s (%s)", obs_encoder_get_display_name(encoderId), encoderId);

	videoEncoder = obs_video_encoder_create(encoderId, "discord golive video", vset, nullptr);
	audioEncoder = obs_audio_encoder_create("ffmpeg_opus", "discord golive audio", aset, 0, nullptr);
	obs_data_release(vset);
	obs_data_release(aset);

	if (!videoEncoder || !audioEncoder) {
		SetStatus(T("DiscordGoLive.Error.Encoders"));
		return false;
	}

	obs_encoder_set_video(videoEncoder, video);
	if (plan.scale)
		obs_encoder_set_scaled_size(videoEncoder, plan.width, plan.height);
	if (plan.divisor > 1)
		obs_encoder_set_frame_rate_divisor(videoEncoder, plan.divisor);
	obs_encoder_set_audio(audioEncoder, audio);

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

	SetStatus(T("DiscordGoLive.Status.Starting"));

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		SetStatus(T("DiscordGoLive.Status.Failed")
				  .arg(err ? QString::fromUtf8(err) : T("DiscordGoLive.Error.Unknown")));
		return false;
	}

	return true;
}

void DiscordDock::StopStream()
{
	if (output && !stopping) {
		stopping = true;
		SetStatus(T("DiscordGoLive.Status.Stopping"));
		obs_output_stop(output);
		UpdateUi();
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

	/* The encoders are gone, so the filter's canvas and audio can go too. */
	if (streamFilter) {
		discord_filter_end(streamFilter);
		obs_source_release(streamFilter);
		streamFilter = nullptr;
	}

	output = nullptr;
	videoEncoder = nullptr;
	audioEncoder = nullptr;
	service = nullptr;
	active = false;
	stopping = false;
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
	SetStatus(T("DiscordGoLive.Status.Live").arg(SelectedChannel().label));
	UpdateUi();
}

void DiscordDock::OnOutputStopped(int code, const QString &error)
{
	if (code == OBS_OUTPUT_SUCCESS)
		SetStatus(T("DiscordGoLive.Status.Idle"));
	else
		SetStatus(T("DiscordGoLive.Status.Failed")
				  .arg(error.isEmpty() ? T("DiscordGoLive.Error.Unknown") : error));

	ReleaseObjects();
	UpdateUi();
}

/* ------------------------------------------------------------------ state */

/* One place decides what is enabled and what the main button says. */
void DiscordDock::UpdateUi()
{
	const bool streaming = output != nullptr;
	const bool busy = account != nullptr;
	const bool editable = !streaming && !busy;

	tokenEdit->setEnabled(editable);
	tokenShow->setEnabled(editable);
	qualityCombo->setEnabled(editable);
	encoderCombo->setEnabled(editable);

	if (busy || connected) {
		/* Disconnect is always available, except while this OBS is streaming (stop that first). */
		connectButton->setText(T("DiscordGoLive.Disconnect"));
		connectButton->setEnabled(!streaming);
	} else {
		connectButton->setText(T("DiscordGoLive.Connect"));
		connectButton->setEnabled(editable && !tokenEdit->text().trimmed().isEmpty());
	}
	refreshButton->setVisible(connected);
	refreshButton->setEnabled(editable);
	channelCombo->setEnabled(editable && connected && channelCombo->count() > 0);

	if (streaming) {
		startStop->setText(T("DiscordGoLive.Stop"));
		startStop->setEnabled(!stopping);
	} else if (remoteStream.valid()) {
		startStop->setText(T("DiscordGoLive.StopRemote").arg(remoteStream.label));
		startStop->setEnabled(!busy);
	} else {
		startStop->setText(T("DiscordGoLive.Start"));
		startStop->setEnabled(!busy && connected && SelectedChannel().valid());
	}
	RequestPropertiesRefresh();
}

void DiscordDock::SetStatus(const QString &text)
{
	status->setText(text);
	RequestPropertiesRefresh();
}

/* ------------------------------------------------------------- filter window */

/* Signals arrive on OBS threads; hop to the UI thread. */
void DiscordDock::FilterRemovedCb(void *data, calldata_t *cd)
{
	auto *window = static_cast<DiscordDock *>(data);
	/* The source lost a filter; only ours matters. Pointers are compared, never used. */
	if (calldata_ptr(cd, "filter") == window->filterPtr)
		QMetaObject::invokeMethod(window, "OnFilterRemoved", Qt::QueuedConnection);
}

void DiscordDock::SourceRemovedCb(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<DiscordDock *>(data), "OnFilterRemoved", Qt::QueuedConnection);
}

/* The filter or its source was removed: stop streaming and close the window for good. */
void DiscordDock::OnFilterRemoved()
{
	Shutdown();
	emit Closed(filterUuid);
	deleteLater();
}

/* ------------------------------------------------- filter properties panel */

/*
 * A filter has no window: its controls are the filter's own OBS properties
 * (the pane on the right of the Filters dialog). This DiscordDock is the
 * filter's hidden controller; the properties are rebuilt from its widgets'
 * state, and their buttons call it. When something visible changes, OBS is
 * asked to rebuild the pane ("update_properties").
 */

/* Joins everything the pane shows, so a refresh happens only when it would change something. */
QString DiscordDock::PropertiesSignature() const
{
	QStringList parts{status->text(), connectButton->text(), startStop->text(),
			  QString::number(connectButton->isEnabled()), QString::number(startStop->isEnabled()),
			  QString::number(connected), QString::number(account != nullptr),
			  QString::number(output != nullptr), QString::number(tokenEdit->text().isEmpty())};
	for (int i = 0; i < channelCombo->count(); i++)
		parts << channelCombo->itemText(i);
	return parts.join(QChar(0x1f));
}

void DiscordDock::RequestPropertiesRefresh()
{
	if (!filterWeak || refreshQueued)
		return;
	refreshQueued = true;
	/* Coalesce bursts of changes into one rebuild, after they settle. */
	QTimer::singleShot(0, this, [this]() {
		refreshQueued = false;
		QString signature = PropertiesSignature();
		if (signature == lastSignature)
			return;
		lastSignature = signature;
		if (obs_source_t *filter = Filter()) {
			obs_source_update_properties(filter);
			obs_source_release(filter);
		}
	});
}

template<void (DiscordDock::*Action)()>
static bool PropsButton(obs_properties_t *, obs_property_t *, void *priv)
{
	(static_cast<DiscordDock *>(priv)->*Action)();
	return false; /* the controller refreshes the pane itself when its state changes */
}

obs_properties_t *DiscordDock::BuildProperties()
{
	lastSignature = PropertiesSignature();
	const bool streaming = output != nullptr;
	const bool busy = account != nullptr;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_text(props, "warning", obs_module_text("DiscordGoLive.TosWarning"),
						    OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);

	/* The token is typed here only until Connect moves it into the token file. */
	if (!connected && !busy) {
		p = obs_properties_add_text(props, S_TOKEN, obs_module_text("DiscordGoLive.Token"), OBS_TEXT_PASSWORD);
		obs_property_set_enabled(p, !streaming);
		if (!tokenEdit->text().isEmpty())
			obs_properties_add_text(props, "token_note", obs_module_text("DiscordGoLive.TokenSaved"),
						OBS_TEXT_INFO);
	}

	p = obs_properties_add_button2(props, "connect", connectButton->text().toUtf8().constData(),
				       PropsButton<&DiscordDock::PropsConnect>, this);
	/* Unlike the dock, the token may still be in the field (not yet in tokenEdit), so only streaming disables it. */
	obs_property_set_enabled(p, !streaming);
	if (connected) {
		p = obs_properties_add_button2(props, "refresh", obs_module_text("DiscordGoLive.Refresh"),
					       PropsButton<&DiscordDock::PropsRefresh>, this);
		obs_property_set_enabled(p, !streaming && !busy);
	}

	p = obs_properties_add_list(props, S_CHANNEL, obs_module_text("DiscordGoLive.VoiceChannel"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (channelCombo->count() == 0) {
		obs_property_list_add_string(p, obs_module_text("DiscordGoLive.ConnectFirst"), "");
	} else {
		obs_property_list_add_string(p, obs_module_text("DiscordGoLive.ChooseChannel"), "");
		for (int i = 0; i < channelCombo->count(); i++) {
			QStringList ids = channelCombo->itemData(i).toStringList();
			if (ids.size() == 2)
				obs_property_list_add_string(p, channelCombo->itemText(i).toUtf8().constData(),
							     QString("%1:%2").arg(ids[0], ids[1]).toUtf8().constData());
		}
	}
	obs_property_set_enabled(p, channelCombo->isEnabled());

	p = obs_properties_add_list(props, S_QUALITY, obs_module_text("DiscordGoLive.Quality"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < qualityCombo->count(); i++)
		obs_property_list_add_int(p, qualityCombo->itemText(i).toUtf8().constData(), i);
	obs_property_set_enabled(p, !streaming && !busy);

	p = obs_properties_add_list(props, S_ENCODER, obs_module_text("DiscordGoLive.Encoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < encoderCombo->count(); i++)
		obs_property_list_add_int(p, encoderCombo->itemText(i).toUtf8().constData(), i);
	obs_property_set_enabled(p, !streaming && !busy);

	p = obs_properties_add_button2(props, "start", startStop->text().toUtf8().constData(),
				       PropsButton<&DiscordDock::OnStartStopClicked>, this);
	obs_property_set_enabled(p, startStop->isEnabled());

	obs_properties_add_text(props, "status", status->text().toUtf8().constData(), OBS_TEXT_INFO);
	obs_properties_add_text(props, "version",
				T("DiscordGoLive.Version").arg(PLUGIN_VERSION, PLUGIN_BUILD_NUMBER).toUtf8().constData(),
				OBS_TEXT_INFO);
	return props;
}

/* Connect (taking a newly typed token out of the settings) or Disconnect. */
void DiscordDock::PropsConnect()
{
	if (!account && !connected) {
		if (obs_source_t *filter = Filter()) {
			obs_data_t *settings = obs_source_get_settings(filter);
			QString typed = QString::fromUtf8(obs_data_get_string(settings, S_TOKEN)).trimmed();
			if (!typed.isEmpty()) {
				tokenEdit->setText(typed); /* saved to the owner-only file by Connect */
				obs_data_set_string(settings, S_TOKEN, "");
			}
			obs_data_release(settings);
			obs_source_release(filter);
		}
	}
	OnConnectClicked();
}

void DiscordDock::PropsRefresh()
{
	RunAccountJob(Job::List);
}

/* The user changed a list in the pane: mirror it into the controller. */
void DiscordDock::ApplyFilterSettings()
{
	obs_source_t *filter = Filter();
	if (!filter)
		return;
	obs_data_t *settings = obs_source_get_settings(filter);

	QStringList ids = QString::fromUtf8(obs_data_get_string(settings, S_CHANNEL)).split(':');
	if (ids.size() == 2) {
		savedGuild = ids[0];
		savedChannel = ids[1];
		for (int i = 0; i < channelCombo->count(); i++) {
			if (channelCombo->itemData(i).toStringList() == ids) {
				const QSignalBlocker block(channelCombo);
				channelCombo->setCurrentIndex(i);
			}
		}
	}
	{
		const QSignalBlocker blockQ(qualityCombo), blockE(encoderCombo);
		qualityCombo->setCurrentIndex((int)obs_data_get_int(settings, S_QUALITY));
		encoderCombo->setCurrentIndex((int)obs_data_get_int(settings, S_ENCODER));
	}

	obs_data_release(settings);
	obs_source_release(filter);
	UpdateUi();
}

/* ------------------------------------------------------------ registration */

static DiscordDock *g_dock = nullptr;
static QHash<QString, QPointer<DiscordDock>> g_windows; /* filter uuid -> its hidden controller */

/* The filter's controller, created on first use. UI thread. */
static DiscordDock *ControllerFor(obs_source_t *filter)
{
	QString uuid = QString::fromUtf8(obs_source_get_uuid(filter));
	QPointer<DiscordDock> controller = g_windows.value(uuid);
	if (!controller) {
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		controller = new DiscordDock(main, filter);
		controller->setWindowFlag(Qt::Window); /* never shown; keeps it out of the main window's layout */
		QObject::connect(controller, &DiscordDock::Closed, [](const QString &id) { g_windows.remove(id); });
		g_windows.insert(uuid, controller);
	}
	return controller;
}

extern "C" obs_properties_t *discord_filter_build_properties(obs_source_t *filter)
{
	/* The controller is a Qt widget: only the UI thread may create or read it. Others
	 * (e.g. obs-websocket) get the warning alone. */
	if (QThread::currentThread() != qApp->thread()) {
		obs_properties_t *props = obs_properties_create();
		obs_properties_add_text(props, "warning", obs_module_text("DiscordGoLive.TosWarning"), OBS_TEXT_INFO);
		return props;
	}
	return ControllerFor(filter)->BuildProperties();
}

extern "C" void discord_filter_settings_changed(obs_source_t *filter)
{
	/* May be called while OBS updates the filter; act once that is done, on the UI thread. */
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main)
		return;
	obs_weak_source_t *weak = obs_source_get_weak_source(filter);
	QMetaObject::invokeMethod(
		main,
		[weak]() {
			if (obs_source_t *source = obs_weak_source_get_source(weak)) {
				ControllerFor(source)->ApplyFilterSettings();
				obs_source_release(source);
			}
			obs_weak_source_release(weak);
		},
		Qt::QueuedConnection);
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_EXIT)
		return;
	if (g_dock)
		g_dock->Shutdown();
	for (const QPointer<DiscordDock> &window : g_windows) {
		if (window)
			window->Shutdown();
	}
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
	for (const QPointer<DiscordDock> &window : g_windows) {
		if (window)
			delete window.data();
	}
	g_windows.clear();
}
