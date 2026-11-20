#pragma once

#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

class AudioFeedback : public QObject {
    Q_OBJECT
public:
    enum class Cue { Start, Stop, Lock, Done, Cancel };

    explicit AudioFeedback(QObject *parent = nullptr);
    ~AudioFeedback() override;

    void play(Cue cue);
    void pauseMedia();
    void resumeMedia();
    bool hasPausedMedia() const { return !m_pausedPlayers.isEmpty(); }
    static QByteArray buildWav(Cue cue);

private:
    QString ensureCueFile(Cue cue);
    QString m_player;
    QStringList m_playerArgs;
    QTemporaryDir m_dir;
    QStringList m_pausedPlayers;
};
