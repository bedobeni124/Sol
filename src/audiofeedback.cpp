#include "audiofeedback.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QtEndian>
#include <cmath>

namespace {
constexpr int kSampleRate = 44100;
constexpr double kPi = 3.14159265358979323846;

struct ToneNote {
    double startSec;
    double durSec;
    double freqStart;
    double freqEnd;
    double gain;
};

void appendU16(QByteArray &out, quint16 v)
{
    char buf[2];
    qToLittleEndian(v, buf);
    out.append(buf, 2);
}

void appendU32(QByteArray &out, quint32 v)
{
    char buf[4];
    qToLittleEndian(v, buf);
    out.append(buf, 4);
}
} // namespace

AudioFeedback::AudioFeedback(QObject *parent)
    : QObject(parent)
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const bool pulseConfigured = qEnvironmentVariableIsSet("PULSE_SERVER")
        || QFileInfo::exists(runtime + "/pulse/native");
    const QString paplay = QStandardPaths::findExecutable("paplay");
    const QString pwplay = QStandardPaths::findExecutable("pw-play");
    const QString aplay = QStandardPaths::findExecutable("aplay");
    if (!paplay.isEmpty() && (pulseConfigured || (pwplay.isEmpty() && aplay.isEmpty()))) {
        m_player = paplay;
        m_playerArgs = {"--client-name=Sol", "--stream-name=Sol Cue", "--volume=42000"};
    } else if (!pwplay.isEmpty() && (QFileInfo::exists(runtime + "/pipewire-0")
                                     || qEnvironmentVariableIsSet("PIPEWIRE_REMOTE") || aplay.isEmpty())) {
        m_player = pwplay;
        m_playerArgs = {"--volume=0.62"};
    } else if (!aplay.isEmpty()) {
        m_player = aplay;
        m_playerArgs = {"-q"};
    }
}

AudioFeedback::~AudioFeedback()
{
    resumeMedia();
}

QByteArray AudioFeedback::buildWav(Cue cue)
{
    // Subtle, warm organic percussion/pop tones (never harsh square-wave beeps).
    QList<ToneNote> notes;
    double totalSec = 0.085;
    switch (cue) {
    case Cue::Start:
        // Warm rising tactile pop
        notes = {
            {0.000, 0.062, 520.0, 690.0, 0.24},
            {0.006, 0.056, 1040.0, 1380.0, 0.07},
        };
        totalSec = 0.075;
        break;
    case Cue::Stop:
        // Soft descending click
        notes = {
            {0.000, 0.058, 620.0, 440.0, 0.22},
            {0.004, 0.048, 1240.0, 880.0, 0.05},
        };
        totalSec = 0.070;
        break;
    case Cue::Lock:
        // Crisp two-step ascending chime for hands-free lock
        notes = {
            {0.000, 0.052, 540.0, 580.0, 0.20},
            {0.045, 0.078, 760.0, 820.0, 0.24},
            {0.045, 0.068, 1520.0, 1640.0, 0.06},
        };
        totalSec = 0.135;
        break;
    case Cue::Done:
        // Very quiet soft confirmation tick
        notes = {
            {0.000, 0.045, 880.0, 940.0, 0.11},
        };
        totalSec = 0.055;
        break;
    case Cue::Cancel:
        // Gentle low muted thud
        notes = {
            {0.000, 0.075, 320.0, 220.0, 0.22},
        };
        totalSec = 0.085;
        break;
    }

    const int numSamples = static_cast<int>(totalSec * kSampleRate);
    QByteArray pcm;
    pcm.resize(numSamples * 2);
    char *dst = pcm.data();
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double sample = 0.0;
        for (const ToneNote &n : notes) {
            if (t < n.startSec || t > n.startSec + n.durSec) continue;
            const double local = t - n.startSec;
            const double progress = local / n.durSec;
            // Smooth Hann-like attack + exponential decay for crisp, click-free sound.
            const double attack = std::min(1.0, local / 0.006);
            const double decay = std::exp(-progress * 4.2) * (1.0 - std::pow(progress, 3.0));
            const double phase = 2.0 * kPi * (n.freqStart * local + 0.5 * (n.freqEnd - n.freqStart) * local * progress);
            sample += std::sin(phase) * n.gain * attack * decay;
        }
        const double clamped = std::max(-0.95, std::min(0.95, sample));
        qToLittleEndian<qint16>(static_cast<qint16>(clamped * 32767.0), dst + i * 2);
    }

    QByteArray wav;
    wav.reserve(44 + pcm.size());
    wav.append("RIFF", 4);
    appendU32(wav, static_cast<quint32>(36 + pcm.size()));
    wav.append("WAVEfmt ", 8);
    appendU32(wav, 16);
    appendU16(wav, 1); // PCM
    appendU16(wav, 1); // Mono
    appendU32(wav, kSampleRate);
    appendU32(wav, kSampleRate * 2);
    appendU16(wav, 2);
    appendU16(wav, 16);
    wav.append("data", 4);
    appendU32(wav, static_cast<quint32>(pcm.size()));
    wav.append(pcm);
    return wav;
}

QString AudioFeedback::ensureCueFile(Cue cue)
{
    if (!m_dir.isValid()) return {};
    const char *name = "start.wav";
    switch (cue) {
    case Cue::Start: name = "start.wav"; break;
    case Cue::Stop: name = "stop.wav"; break;
    case Cue::Lock: name = "lock.wav"; break;
    case Cue::Done: name = "done.wav"; break;
    case Cue::Cancel: name = "cancel.wav"; break;
    }
    const QString path = m_dir.filePath(name);
    if (!QFileInfo::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(buildWav(cue));
    }
    return path;
}

void AudioFeedback::play(Cue cue)
{
    if (m_player.isEmpty()) return;
    const QString path = ensureCueFile(cue);
    if (path.isEmpty()) return;
    QStringList args = m_playerArgs;
    args.append(path);
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
    proc->start(m_player, args);
}

void AudioFeedback::pauseMedia()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) return;
    const QDBusReply<QStringList> names = bus.interface()->registeredServiceNames();
    if (!names.isValid()) return;
    for (const QString &service : names.value()) {
        if (!service.startsWith("org.mpris.MediaPlayer2.")) continue;
        if (m_pausedPlayers.contains(service)) continue;
        QDBusInterface props(service, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", bus);
        props.setTimeout(150);
        const QDBusMessage reply = props.call("Get", "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) continue;
        const QString status = reply.arguments().first().value<QDBusVariant>().variant().toString();
        if (status == "Playing") {
            QDBusMessage pauseMsg = QDBusMessage::createMethodCall(
                service, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Pause");
            bus.call(pauseMsg, QDBus::NoBlock);
            m_pausedPlayers.append(service);
        }
    }
}

void AudioFeedback::resumeMedia()
{
    if (m_pausedPlayers.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    const QStringList players = std::exchange(m_pausedPlayers, {});
    if (!bus.isConnected()) return;
    for (const QString &service : players) {
        QDBusMessage playMsg = QDBusMessage::createMethodCall(
            service, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "Play");
        bus.call(playMsg, QDBus::NoBlock);
    }
}
