#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>

class Desktop : public QObject {
    Q_OBJECT
public:
    explicit Desktop(QObject *parent = nullptr);
    ~Desktop() override;

    void initialize(const QString &holdChord = {});
    void configure();
    void configureInput();
    void rebindX11(const QString &holdChord);
    void request(const QJsonObject &job, std::function<void(QJsonObject)> finished);
    void indicate(const QJsonObject &hudState);
    void notify(const QString &message, bool transient = false);
    bool isX11() const { return x11Session; }
    bool isWayland() const { return waylandSession; }
    QString desktopEnvironment() const;
    QString shortcutSnippet(const QString &holdChord = "F9") const;

    // Universal chord conversion & live shortcut management across Linux desktops
    static QString toHyprChord(const QString &chord);
    static QString toHyprLuaChord(const QString &chord);
    static QString toSwayChord(const QString &chord);
    static QString toNiriChord(const QString &chord);
    static QString toGnomeChord(const QString &chord);
    static QString toPortalChord(const QString &chord);
    static bool isHyprlandLua(const QString &configHomeOverride = {});
    static QString hyprLuaScript(const QString &holdChord, bool enableGlassBlur = false);
    static bool hasActiveHyprlandBinds();
    static bool applyLiveShortcuts(const QString &holdChord, bool enableGlassBlur = false, QString *summaryOut = nullptr);
    static bool persistShortcuts(const QString &holdChord, bool enableGlassBlur = false, QString *summaryOut = nullptr);
    static void removePersistedShortcuts(const QString &configHomeOverride = {});

    QString info, hold, inputInfo;

signals:
    void shortcutsChanged();
    void action(QString command);
    void hudMessage(QJsonObject message);

private:
    void ensureDesktopWorker();
    void ensureHud(const QString &theme);
    void handleWorkerResponse(const QJsonObject &msg);

    QProcess shortcuts, hud, input, desktopWorker;
    QTemporaryDir scripts;
    bool desktopWorkerReady = false;
    bool portalReady = false;
    bool x11Session = false;
    bool waylandSession = false;
    bool nativeKeys = false;
    bool hudAvailable = false;
    bool hudUseXcb = false;
    bool hudFailed = false;
    int nextRequest = 0;
    quint32 notifyId = 0;
    QString lastPhase = "idle";
    QString activeHudTheme;
    struct PendingJob {
        QJsonObject job;
        std::function<void(QJsonObject)> callback;
        QTimer *timer = nullptr;
    };
    QMap<int, PendingJob> workerJobs;
    QMap<int, std::function<void(QJsonObject)>> deliveries;
    QList<QPair<int, QJsonObject>> workerQueue;
};
