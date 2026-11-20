#pragma once

#include "audiofeedback.h"
#include "desktop.h"
#include "formatter.h"
#include "speechengine.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class Backend : public QObject {
    Q_OBJECT
public:
    explicit Backend(bool interactive, QObject *parent = nullptr, const QString &dataDirOverride = {});

    QString state() const { return phase; }
    QString activeMode() const { return mode; }
    QString message() const { return notice; }
    QString lastText() const { return latest; }
    qreal level() const { return rms; }
    bool canRetryLast() const { return speech.hasLastRecording() && phase == "idle"; }

    QString language() const { return data.value("language").toString("auto"); }
    QStringList preferredLanguages() const;
    QString languagesDisplay() const;
    QString style() const { return data.value("style").toString("auto"); }
    QString cleanup() const { return data.value("cleanup").toString("light"); }
    QString personalStyle() const { return data.value("personalStyle").toString("casual"); }
    QString workStyle() const { return data.value("workStyle").toString("formal"); }
    QString emailStyle() const { return data.value("emailStyle").toString("formal"); }
    QString codeStyle() const { return data.value("codeStyle").toString("formal"); }
    QString otherStyle() const { return data.value("otherStyle").toString("formal"); }
    QVariantList appRules() const { return data.value("appRules").toVariant().toList(); }
    QString codeMode() const { return data.value("codeMode").toString("auto"); }
    bool fileTagging() const { return data.value("fileTagging").toBool(true); }
    bool whisperMode() const { return data.value("whisperMode").toBool(false); }

    QString microphone() const { return data.value("microphone").toString("default"); }
    QVariantList microphones() const { return micList; }
    bool soundEffects() const { return data.value("soundEffects").toBool(true); }
    bool pauseMedia() const { return data.value("pauseMedia").toBool(false); }
    bool restoreClipboard() const { return data.value("restoreClipboard").toBool(true); }
    bool doubleTapLock() const { return data.value("doubleTapLock").toBool(true); }
    bool showHud() const;
    QString hudTheme() const;
    bool keepHistory() const { return data.value("keepHistory").toBool(true); }
    bool autoInsert() const { return data.value("autoInsert").toBool(true); }
    bool contextAware() const { return data.value("contextAware").toBool(true); }
    bool autostart() const;

    QVariantList history() const { return data.value("history").toVariant().toList(); }
    QVariantList dictionary() const { return data.value("dictionary").toVariant().toList(); }
    QVariantList snippets() const { return data.value("snippets").toVariant().toList(); }
    int totalWords() const;
    int totalDictations() const;
    int averageWpm() const;
    int streakDays() const;
    int minutesSaved() const;

    QString shortcut() const;
    QString activeShortcut() const { return desktop.hold; }
    QString desktopName() const { return desktop.desktopEnvironment(); }
    bool isX11() const { return desktop.isX11(); }

    bool speechReady() const { return speech.ready(); }
    QString speechModel() const { return speech.activeModel(); }
    QString speechDevice() const { return speech.activeDevice(); }

    QVariant optionValue(const QString &key) const;
    void toggle();
    void cancel();
    void pasteLast();
    void retryLast();
    bool addEntry(const QString &kind, const QString &trigger, const QString &value);
    bool removeEntry(const QString &kind, const QString &trigger);
    bool addAppRule(const QString &appPattern, const QString &styleValue);
    bool removeAppRule(const QString &appPattern);
    bool setLanguages(const QString &lang, const QStringList &preferred);
    bool editHistory(const QString &id, const QString &text);
    bool revertToRaw(const QString &id);
    bool reformatHistory(const QString &id, const QString &targetStyle);
    void removeHistory(const QString &id);
    void clearHistory();
    void setOption(const QString &key, const QVariant &value);
    void toggleOption(const QString &key);
    void refreshMicrophones();
    void configureShortcuts() { desktop.configure(); }
    void configureInput() { desktop.configureInput(); }
    QString previewStyle(const QString &sample, const QString &tone) const;

    void control(const QString &action);
    void reloadFromDisk();
    void transcribe(const QString &path, const QString &languageOverride = {},
                    const QString &styleOverride = {}, const QString &cleanupOverride = {});
    QJsonObject status() const;

    QString resolveAppStyle(const QJsonObject &focusTarget) const;
    bool isCodeContext(const QJsonObject &focusTarget) const;
    bool isUrlBarContext(const QJsonObject &focusTarget) const;

signals:
    void changed();
    void volumeChanged();
    void settingsChanged();
    void shortcutChanged();
    void microphonesChanged();
    void completed(QString text, QString error);

private:
    void begin(bool fromShortcut, const QString &requestedMode);
    void stop();
    void stopNow();
    void setPhase(const QString &value);
    void pushHud(const QString &toast = {}, const QString &toastKind = "info");
    void showHudToast(const QString &toast, const QString &kind = "ok", int ms = 1200);
    void say(const QString &text);
    void handleHudMessage(const QJsonObject &msg);
    void finish(const QString &formatted);
    void fail(const QString &reason);
    void maybeStartPending();
    bool save(const QJsonObject &next);
    void recordStats(QJsonObject &next, const QString &text, qreal durationSec) const;
    QString autostartPath() const;
    bool writeAutostart(bool enable);
    QString prompt() const;
    QStringList contextSymbols() const;
    QString appStyle() const;
    QString matchContext(QString text) const;
    bool learn(QJsonObject &next, const QString &oldText, const QString &newText) const;

    bool gui, external = false, writing = false, corrupt = false, overrideActive = false;
    bool pendingStart = false, hudEditing = false, hudHovered = false;
    QString phase = "idle", mode = "ptt", notice, latest, raw, detected;
    QString storage, autostartFileOverride, entryId, priorText;
    QString hudToast, hudToastKind = "info";
    QString styleOverrideValue, cleanupOverrideValue;
    QJsonObject data, target, priorTarget;
    QVariantList micList;
    SpeechEngine speech;
    Desktop desktop;
    AudioFeedback audio;
    QElapsedTimer timer, pressTimer;
    QTimer doubleTapTimer, hudToastTimer;
    qreal rms = 0, duration = 0;
    quint64 generation = 0;
};
