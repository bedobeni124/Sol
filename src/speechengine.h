#pragma once

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class QProcess;

class SpeechEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit SpeechEngine(QObject *parent = nullptr);
    ~SpeechEngine() override;

    bool ready() const;
    QString activeModel() const { return m_activeModel; }
    QString activeDevice() const { return m_activeDevice; }
    QString detectedLanguage() const { return m_detectedLanguage; }
    bool hasLastRecording() const { return !m_lastPcm.isEmpty(); }
    void warmUp();
    void start(const QString &language, const QStringList &preferredLanguages,
               const QString &prompt, const QString &microphone, bool whisperMode);
    void setPrompt(const QString &prompt);
    void stop();
    void cancel();
    void transcribeFile(const QString &path, const QString &language,
                        const QStringList &preferredLanguages, const QString &prompt,
                        bool whisperMode = false);
    void retranscribeLast(const QString &language, const QStringList &preferredLanguages,
                          const QString &prompt, bool whisperMode = false);
    void startMicTest(const QString &microphone);
    void stopMicTest();
    bool testingMic() const { return m_state == State::MicTest; }
    static QVariantList listMicrophones();

signals:
    void recordingStarted();
    void processingStarted();
    void partialResult(const QString &text);
    void result(const QString &text);
    void failed(const QString &message);
    void levelChanged(qreal level);
    void readyChanged();

private:
    enum class State { Idle, Recording, Stopping, Processing, MicTest };
    bool checkSetup();
    bool resolveCaptureCommand(const QString &microphone, QString &program, QStringList &args) const;
    void readEngineMetadata();
    void ensureWorker();
    void sendJob();
    bool writeAudioJob(const QJsonObject &job, const QByteArray &pcm);
    void appendSegment(const QString &text, const QString &language);
    void readWorker();
    void readAudio();
    void finishCapture();
    void fail(const QString &message);
    void releaseProcess(QProcess *&process);

    QString m_root;
    QFileSystemWatcher m_watcher;
    QProcess *m_capture = nullptr;
    QProcess *m_worker = nullptr;
    QTimer m_stopGuard;
    QByteArray m_pcm;
    QByteArray m_lastPcm;
    QString m_segments;
    qsizetype m_quietSamples = 0;
    qsizetype m_longestSegment = 0;
    bool m_sentPartial = false;
    bool m_partialPending = false;
    bool m_whisperMode = false;
    QJsonObject m_job;
    QString m_detectedLanguage;
    QString m_activeModel;
    QString m_activeDevice;
    QString m_workerError;
    QByteArray m_workerConfig;
    bool m_workerReady = false;
    State m_state = State::Idle;
};
