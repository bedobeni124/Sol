#include "speechengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>
#include <cmath>

namespace {
constexpr qsizetype kMaxSavedPcmBytes = 120 * 32000; // 120 seconds @ 16kHz 16-bit mono (~3.8 MB)

QJsonObject makeJob(const QString &language, const QStringList &preferredLanguages,
                    const QString &prompt, bool whisperMode)
{
    QString code = language.trimmed().toLower().section('-', 0, 0).section('_', 0, 0);
    if (code == "auto") code.clear();
    QJsonArray pref;
    for (const QString &p : preferredLanguages) {
        const QString clean = p.trimmed().toLower().section('-', 0, 0).section('_', 0, 0);
        if (!clean.isEmpty() && clean != "auto") pref.append(clean);
    }
    return {
        {"language", code},
        {"languages", pref},
        {"prompt", prompt.left(2048)},
        {"whisper", whisperMode},
    };
}
} // namespace

SpeechEngine::SpeechEngine(QObject *parent)
    : QObject(parent),
      m_root(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sol/speech")
{
    QDir().mkpath(m_root);
    m_watcher.addPath(m_root);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        readEngineMetadata();
        emit readyChanged();
    });
    m_stopGuard.setSingleShot(true);
    m_stopGuard.setInterval(800);
    connect(&m_stopGuard, &QTimer::timeout, this, [this] {
        if (m_state == State::Stopping && m_capture && m_capture->state() != QProcess::NotRunning)
            m_capture->kill();
    });
    readEngineMetadata();
}

SpeechEngine::~SpeechEngine()
{
    m_stopGuard.stop();
    releaseProcess(m_capture);
    releaseProcess(m_worker);
}

void SpeechEngine::readEngineMetadata()
{
    QFile config(m_root + "/engine.json");
    if (!config.open(QIODevice::ReadOnly)) {
        m_activeModel.clear();
        m_activeDevice.clear();
        return;
    }
    const auto settings = QJsonDocument::fromJson(config.readAll()).object();
    m_activeModel = settings.value("model").toString();
    m_activeDevice = settings.value("device").toString();
}

bool SpeechEngine::ready() const
{
    QFile config(m_root + "/engine.json");
    if (!config.open(QIODevice::ReadOnly)) return false;
    const auto settings = QJsonDocument::fromJson(config.readAll()).object();
    const QString model = settings.value("model").toString();
    return !model.isEmpty() && QFileInfo::exists(m_root + "/models/" + model + "/model.bin")
           && QFileInfo(m_root + "/runtime-v1/bin/python").isExecutable();
}

bool SpeechEngine::checkSetup()
{
    if (ready()) return true;
    fail(tr("Install the speech model first, then try again."));
    return false;
}

void SpeechEngine::releaseProcess(QProcess *&process)
{
    if (!process) return;
    QProcess *old = process;
    process = nullptr;
    old->disconnect(this);
    if (old->state() == QProcess::Starting) old->waitForStarted(1000);
    if (old->state() != QProcess::NotRunning) {
        old->kill();
        old->waitForFinished(2000);
    }
    old->deleteLater();
}

void SpeechEngine::warmUp()
{
    readEngineMetadata();
    if (ready()) ensureWorker();
}

void SpeechEngine::ensureWorker()
{
    QFile config(m_root + "/engine.json");
    if (!config.open(QIODevice::ReadOnly)) {
        fail(tr("Could not read the speech model settings. Run setup-speech again."));
        return;
    }
    const QByteArray currentConfig = config.readAll();
    if (m_worker && m_workerConfig == currentConfig) return;
    releaseProcess(m_worker);
    m_workerConfig = currentConfig;
    readEngineMetadata();

    QFile workerCode(":/speech/worker.py");
    if (!workerCode.open(QIODevice::ReadOnly)) {
        fail(tr("The speech worker is missing from the Sol installation."));
        return;
    }

    m_workerReady = false;
    m_workerError.clear();
    auto *worker = new QProcess(this);
    m_worker = worker;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("HF_HUB_OFFLINE", "1");
    env.insert("HF_HUB_DISABLE_TELEMETRY", "1");
    env.insert("PYTHONUTF8", "1");
    env.remove("PYTHONPATH");
    env.remove("PYTHONHOME");
    worker->setProcessEnvironment(env);
    connect(worker, &QProcess::readyReadStandardOutput, this, &SpeechEngine::readWorker);
    connect(worker, &QProcess::readyReadStandardError, this, [this, worker] {
        m_workerError = (m_workerError + QString::fromUtf8(worker->readAllStandardError())).right(1600);
    });
    auto workerLost = [this, worker] {
        if (worker != m_worker) return;
        QString reason = (m_workerError + QString::fromUtf8(worker->readAllStandardError())).trimmed();
        if (reason.isEmpty()) reason = worker->errorString();
        m_worker = nullptr;
        m_workerReady = false;
        worker->deleteLater();
        if (m_state != State::Idle && m_state != State::MicTest)
            fail(tr("The speech engine stopped. %1").arg(reason));
    };
    connect(worker, &QProcess::finished, this, workerLost);
    connect(worker, &QProcess::errorOccurred, this, [workerLost](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) workerLost();
    });
    worker->start(m_root + "/runtime-v1/bin/python",
                  {"-u", "-c", QString::fromUtf8(workerCode.readAll()), m_root});
}

bool SpeechEngine::writeAudioJob(const QJsonObject &job, const QByteArray &pcm)
{
    const QByteArray header = QJsonDocument(job).toJson(QJsonDocument::Compact) + '\n';
    if (!m_worker || m_worker->write(header) < 0 || (!pcm.isEmpty() && m_worker->write(pcm) < 0)) {
        releaseProcess(m_worker);
        m_workerReady = false;
        fail(tr("Could not send audio to the speech engine."));
        return false;
    }
    return true;
}

void SpeechEngine::sendJob()
{
    if (m_state != State::Processing || !m_workerReady || m_job.isEmpty()) return;
    QByteArray payload = m_pcm;
    if (writeAudioJob(m_job, payload)) {
        m_pcm.clear();
        m_job = {};
    }
}

void SpeechEngine::appendSegment(const QString &text, const QString &language)
{
    if (text.isEmpty()) return;
    static const QStringList noWordSpace = {"zh", "ja", "th", "lo", "km", "my"};
    if (!m_segments.isEmpty() && !noWordSpace.contains(language)) m_segments += ' ';
    m_segments += text;
    if (text.size() > m_longestSegment) {
        m_longestSegment = text.size();
        m_detectedLanguage = language;
    }
}

void SpeechEngine::readWorker()
{
    auto *worker = m_worker;
    while (worker == m_worker && worker->canReadLine()) {
        const auto message = QJsonDocument::fromJson(worker->readLine()).object();
        if (message.value("ready").toBool()) {
            m_workerReady = true;
            if (message.contains("model")) m_activeModel = message.value("model").toString();
            if (message.contains("device")) m_activeDevice = message.value("device").toString();
            emit readyChanged();
            sendJob();
        } else if (message.value("partial").toBool() || m_state == State::Processing) {
            if (message.contains("error")) {
                // A final job may already be queued behind the failed chunk.
                releaseProcess(m_worker);
                m_workerReady = false;
                fail(tr("Transcription failed. %1").arg(message.value("error").toString()));
                return;
            }
            appendSegment(message.value("text").toString(), message.value("language").toString());
            if (message.value("partial").toBool()) {
                m_partialPending = false;
                if (!m_segments.isEmpty()) emit partialResult(m_segments);
            } else if (m_segments.isEmpty()) {
                fail(tr("No speech detected."));
            } else {
                const QString finalText = m_segments;
                m_segments.clear();
                m_state = State::Idle;
                emit result(finalText);
            }
        }
    }
}

bool SpeechEngine::resolveCaptureCommand(const QString &microphone, QString &program, QStringList &args) const
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const bool pulseConfigured = qEnvironmentVariableIsSet("PULSE_SERVER")
        || qEnvironmentVariableIsSet("PULSE_SOURCE") || QFileInfo::exists(runtime + "/pulse/native");
    const QString parec = QStandardPaths::findExecutable("parec");
    const QString pipewire = QStandardPaths::findExecutable("pw-record");
    const QString alsa = QStandardPaths::findExecutable("arecord");
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    const QString mic = (microphone == "default" || microphone == "auto") ? QString() : microphone.trimmed();

    if (!parec.isEmpty() && (pulseConfigured || (pipewire.isEmpty() && alsa.isEmpty()))) {
        program = parec;
        args = {"--raw", "--format=s16le", "--rate=16000", "--channels=1",
                "--latency-msec=20", "--process-time-msec=10",
                "--client-name=Sol", "--stream-name=Sol Dictation"};
        if (!mic.isEmpty()) args << ("--device=" + mic);
        return true;
    }
    if (!pipewire.isEmpty() && (QFileInfo::exists(runtime + "/pipewire-0")
                                || qEnvironmentVariableIsSet("PIPEWIRE_REMOTE") || alsa.isEmpty())) {
        program = pipewire;
        args = {"--raw", "--format=s16", "--rate=16000", "--channels=1", "--latency=20ms"};
        if (!mic.isEmpty()) args << ("--target=" + mic);
        args << "-";
        return true;
    }
    if (!alsa.isEmpty()) {
        program = alsa;
        args = {"-q", "-t", "raw", "-f", "S16_LE", "-r", "16000", "-c", "1"};
        if (!mic.isEmpty()) args << "-D" << mic;
        return true;
    }
    if (!ffmpeg.isEmpty()) {
        program = ffmpeg;
        args = {"-nostdin", "-loglevel", "error", "-f", pulseConfigured ? "pulse" : "alsa",
                "-i", mic.isEmpty() ? "default" : mic, "-ac", "1", "-ar", "16000", "-f", "s16le", "-"};
        return true;
    }
    return false;
}

void SpeechEngine::start(const QString &language, const QStringList &preferredLanguages,
                         const QString &prompt, const QString &microphone, bool whisperMode)
{
    if (m_state == State::MicTest) stopMicTest();
    if (m_state != State::Idle || !checkSetup()) return;
    m_whisperMode = whisperMode;
    m_job = makeJob(language, preferredLanguages, prompt, whisperMode);
    m_pcm.clear();
    m_lastPcm.clear();
    m_segments.clear();
    m_quietSamples = m_longestSegment = 0;
    m_partialPending = m_sentPartial = false;
    m_detectedLanguage.clear();

    QString program;
    QStringList args;
    if (!resolveCaptureCommand(microphone, program, args)) {
        fail(tr("Install a microphone capture tool: parec, pw-record, or arecord."));
        return;
    }
    auto *capture = new QProcess(this);
    m_capture = capture;
    m_state = State::Recording;
    connect(capture, &QProcess::started, this, [this, capture] {
        if (m_state == State::Stopping) {
            capture->terminate();
            m_stopGuard.start();
        } else {
            emit recordingStarted();
        }
    });
    connect(capture, &QProcess::readyReadStandardOutput, this, &SpeechEngine::readAudio);
    connect(capture, &QProcess::finished, this, [this, capture] {
        m_stopGuard.stop();
        if (m_state == State::Stopping) {
            finishCapture();
        } else {
            const QString reason = QString::fromUtf8(capture->readAllStandardError()).trimmed();
            fail(tr("Microphone capture stopped. %1").arg(reason));
        }
    });
    connect(capture, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(tr("Could not start the microphone capture tool."));
    });
    capture->start(program, args);
    ensureWorker();
}

void SpeechEngine::startMicTest(const QString &microphone)
{
    if (m_state == State::MicTest) stopMicTest();
    if (m_state != State::Idle) return;
    QString program;
    QStringList args;
    if (!resolveCaptureCommand(microphone, program, args)) return;
    auto *capture = new QProcess(this);
    m_capture = capture;
    m_state = State::MicTest;
    connect(capture, &QProcess::readyReadStandardOutput, this, &SpeechEngine::readAudio);
    connect(capture, &QProcess::finished, this, [this] {
        if (m_state == State::MicTest) {
            releaseProcess(m_capture);
            m_state = State::Idle;
            emit levelChanged(0);
        }
    });
    capture->start(program, args);
}

void SpeechEngine::stopMicTest()
{
    if (m_state != State::MicTest) return;
    releaseProcess(m_capture);
    m_state = State::Idle;
    emit levelChanged(0);
}

void SpeechEngine::readAudio()
{
    if (!m_capture) return;
    const QByteArray chunk = m_capture->readAllStandardOutput();
    if (chunk.size() < 2) {
        if (m_state != State::MicTest) m_pcm.append(chunk);
        return;
    }
    double squares = 0;
    for (qsizetype i = 0; i + 1 < chunk.size(); i += 2) {
        const double sample = qFromLittleEndian<qint16>(chunk.constData() + i) / 32768.0;
        squares += sample * sample;
    }
    const double rms = std::sqrt(squares / (chunk.size() / 2));
    // Boost visual reactivity slightly in whisper mode so quiet speaking animates smoothly.
    const double displayRms = m_whisperMode ? rms * 3.5 : rms;
    const qreal normalized = qBound(0., (20 * std::log10(qMax(1e-6, displayRms)) + 60.) / 60., 1.);

    if (m_state == State::MicTest) {
        emit levelChanged(normalized);
        return;
    }

    m_pcm.append(chunk);
    if (m_lastPcm.size() < kMaxSavedPcmBytes)
        m_lastPcm.append(chunk.left(kMaxSavedPcmBytes - m_lastPcm.size()));

    const double quietThreshold = m_whisperMode ? 0.0025 : 0.007;
    const qsizetype quietWindow = m_whisperMode ? 6400 : 8000; // 400-500ms natural pause
    m_quietSamples = rms < quietThreshold ? m_quietSamples + chunk.size() / 2 : 0;

    // Pre-transcribe long dictations at natural pauses (every ~10s) so final insertion feels instant.
    if (m_state == State::Recording && m_workerReady && !m_partialPending
        && m_pcm.size() >= 10 * 32000 && m_quietSamples >= quietWindow && m_pcm.size() % 2 == 0) {
        QJsonObject part = m_job;
        part["partial"] = true;
        part["bytes"] = qint64(m_pcm.size());
        if (!writeAudioJob(part, m_pcm)) return;
        m_pcm.clear();
        m_sentPartial = m_partialPending = true;
        m_quietSamples = 0;
    }
    emit levelChanged(normalized);
}

void SpeechEngine::setPrompt(const QString &prompt)
{
    if (m_state == State::Recording || m_state == State::Stopping)
        m_job["prompt"] = prompt.left(2048);
}

void SpeechEngine::stop()
{
    if (m_state != State::Recording) return;
    m_state = State::Stopping;
    if (m_capture && m_capture->state() == QProcess::Running) {
        m_capture->terminate();
        m_stopGuard.start();
    }
}

void SpeechEngine::finishCapture()
{
    readAudio();
    const QString captureError = m_capture ? QString::fromUtf8(m_capture->readAllStandardError()).trimmed() : QString();
    releaseProcess(m_capture);
    emit levelChanged(0);
    if (m_pcm.size() % 2) m_pcm.chop(1);
    if (m_pcm.size() < 1600 && !m_sentPartial) {
        fail(captureError.isEmpty() ? tr("Recording too short.")
                                    : tr("Microphone error. %1").arg(captureError));
        return;
    }
    m_job["bytes"] = qint64(m_pcm.size());
    m_state = State::Processing;
    emit processingStarted();
    sendJob();
}

void SpeechEngine::transcribeFile(const QString &path, const QString &language,
                                  const QStringList &preferredLanguages, const QString &prompt,
                                  bool whisperMode)
{
    if (m_state == State::MicTest) stopMicTest();
    if (m_state != State::Idle || !checkSetup()) return;
    QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        fail(tr("Choose a readable audio file."));
        return;
    }
    m_whisperMode = whisperMode;
    m_pcm.clear();
    m_segments.clear();
    m_quietSamples = m_longestSegment = 0;
    m_partialPending = m_sentPartial = false;
    m_detectedLanguage.clear();
    m_job = makeJob(language, preferredLanguages, prompt, whisperMode);
    m_job["path"] = file.absoluteFilePath();
    m_state = State::Processing;
    emit processingStarted();
    ensureWorker();
    sendJob();
}

void SpeechEngine::retranscribeLast(const QString &language, const QStringList &preferredLanguages,
                                    const QString &prompt, bool whisperMode)
{
    if (m_state == State::MicTest) stopMicTest();
    if (m_state != State::Idle || !checkSetup()) return;
    if (m_lastPcm.size() < 1600) {
        fail(tr("No recent recording is available to retry."));
        return;
    }
    m_whisperMode = whisperMode;
    m_pcm = m_lastPcm;
    if (m_pcm.size() % 2) m_pcm.chop(1);
    m_segments.clear();
    m_quietSamples = m_longestSegment = 0;
    m_partialPending = m_sentPartial = false;
    m_detectedLanguage.clear();
    m_job = makeJob(language, preferredLanguages, prompt, whisperMode);
    m_job["bytes"] = qint64(m_pcm.size());
    m_state = State::Processing;
    emit processingStarted();
    ensureWorker();
    sendJob();
}

void SpeechEngine::cancel()
{
    m_stopGuard.stop();
    if (m_state == State::Processing || m_partialPending) {
        releaseProcess(m_worker);
        m_workerReady = false;
    }
    releaseProcess(m_capture);
    m_state = State::Idle;
    m_pcm.clear();
    m_segments.clear();
    m_partialPending = false;
    m_job = {};
    emit levelChanged(0);
}

void SpeechEngine::fail(const QString &message)
{
    m_stopGuard.stop();
    if (m_partialPending) {
        releaseProcess(m_worker);
        m_workerReady = false;
    }
    releaseProcess(m_capture);
    m_state = State::Idle;
    m_pcm.clear();
    m_segments.clear();
    m_partialPending = false;
    m_job = {};
    emit levelChanged(0);
    emit failed(message);
}

QVariantList SpeechEngine::listMicrophones()
{
    QVariantList devices;
    devices.append(QVariantMap{{"id", "default"}, {"label", tr("System default")}});

    const QString pactl = QStandardPaths::findExecutable("pactl");
    if (!pactl.isEmpty()) {
        QProcess proc;
        proc.start(pactl, {"-f", "json", "list", "sources"});
        if (proc.waitForFinished(1200) && proc.exitCode() == 0) {
            const auto array = QJsonDocument::fromJson(proc.readAllStandardOutput()).array();
            for (const auto &item : array) {
                const auto obj = item.toObject();
                const QString name = obj.value("name").toString();
                if (name.isEmpty() || name.endsWith(".monitor")) continue;
                QString desc = obj.value("description").toString();
                if (desc.isEmpty()) desc = name;
                devices.append(QVariantMap{{"id", name}, {"label", desc}});
            }
            if (devices.size() > 1) return devices;
        }
        // Fallback for older pactl without -f json
        QProcess textProc;
        textProc.start(pactl, {"list", "short", "sources"});
        if (textProc.waitForFinished(1200) && textProc.exitCode() == 0) {
            for (const QString &line : QString::fromUtf8(textProc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
                const QString name = line.section('\t', 1, 1).trimmed();
                if (name.isEmpty() || name.endsWith(".monitor")) continue;
                devices.append(QVariantMap{{"id", name}, {"label", name}});
            }
            if (devices.size() > 1) return devices;
        }
    }

    const QString arecord = QStandardPaths::findExecutable("arecord");
    if (!arecord.isEmpty()) {
        QProcess proc;
        proc.start(arecord, {"-L"});
        if (proc.waitForFinished(1200) && proc.exitCode() == 0) {
            QString currentId;
            for (const QString &raw : QString::fromUtf8(proc.readAllStandardOutput()).split('\n')) {
                if (!raw.startsWith(' ') && !raw.startsWith('\t') && !raw.trimmed().isEmpty()) {
                    currentId = raw.trimmed();
                    if (currentId.startsWith("plughw:") || currentId.startsWith("sysdefault:") || currentId == "pipewire" || currentId == "pulse")
                        devices.append(QVariantMap{{"id", currentId}, {"label", currentId}});
                } else if (!currentId.isEmpty() && !devices.isEmpty() && devices.last().toMap().value("id").toString() == currentId) {
                    QVariantMap m = devices.last().toMap();
                    m["label"] = raw.trimmed() + " (" + currentId + ")";
                    devices.last() = m;
                    currentId.clear();
                }
            }
        }
    }
    return devices;
}
