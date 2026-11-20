#include "backend.h"
#include "cli_ui.h"
#include "desktop.h"
#include "hudwindow.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#ifdef SOL_LAYER_SHELL
#include <LayerShellQt/Shell>
#include <LayerShellQt/Window>
#endif

namespace {
constexpr auto service = "org.sol.Dictation";
constexpr auto object = "/org/sol/Dictation";

void metadata()
{
    QCoreApplication::setApplicationName("sol");
    QCoreApplication::setApplicationVersion("0.2.0");
}

QString daemonLogPath()
{
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state") + "/sol";
    QDir().mkpath(state);
    return state + "/daemon.log";
}

class Control : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.sol.Dictation")
public:
    Backend *backend = nullptr;
public slots:
    QString Command(const QString &action)
    {
        if (action == "reload") {
            backend->reloadFromDisk();
        } else if (action == "configure-shortcuts") {
            backend->configureShortcuts();
        } else if (action == "configure-input") {
            backend->configureInput();
        } else if (action == "retry") {
            backend->retryLast();
        } else if (action == "quit") {
            if (backend->state() != "idle")
                return "A dictation is active. Finish or cancel it before quitting.";
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        } else if (action == "status") {
            return QString::fromUtf8(QJsonDocument(backend->status()).toJson(QJsonDocument::Compact));
        } else {
            backend->control(action);
        }
        return {};
    }
};

class HudBridge : public QObject {
    Q_OBJECT
public slots:
    void sendMessage(const QJsonObject &msg)
    {
        const QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
        if (::write(STDOUT_FILENO, line.constData(), line.size()) < 0) {}
    }
};

QString findScript(const QCoreApplication &app, const QString &installedName, const QString &relativeName)
{
    QString script = QStandardPaths::findExecutable(installedName);
    if (!script.isEmpty()) return script;
    const QStringList suffixes = {
        "/../libexec/sol/" + relativeName,
        "/../lib/sol/" + relativeName,
        "/../bin/" + relativeName,
        "/" + relativeName,
    };
    for (const QString &suffix : suffixes) {
        const QString p = app.applicationDirPath() + suffix;
        if (QFileInfo(p).isExecutable()) return p;
    }
    return {};
}

bool isDaemonRunning()
{
    auto bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(service).value();
}

bool notifyDaemon(const QString &action, QString *replyOut = nullptr)
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface() || !bus.interface()->isServiceRegistered(service).value())
        return false;
    QDBusInterface remote(service, object, service, bus);
    QDBusReply<QString> reply = remote.call("Command", action);
    if (!reply.isValid()) return false;
    if (replyOut) *replyOut = reply.value();
    return true;
}

bool startDaemonSilent(const QCoreApplication &app, QTextStream &err)
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        err << "  " << CliUi::bad("[ERR]") << " Session D-Bus unavailable. Use headless commands like "
            << CliUi::cmd("sol transcribe") << " or " << CliUi::cmd("sol settings") << ".\n";
        return false;
    }
    if (isDaemonRunning()) return true;

    QProcess launcher;
    launcher.setProgram(app.applicationFilePath());
    launcher.setArguments({"daemon"});
    const QString logFile = daemonLogPath();
    launcher.setStandardOutputFile(logFile, QIODevice::Append);
    launcher.setStandardErrorFile(logFile, QIODevice::Append);
    launcher.setStandardInputFile(QProcess::nullDevice());
    if (!launcher.startDetached()) {
        err << "  " << CliUi::bad("[ERR]") << " Could not start Sol daemon.\n";
        return false;
    }
    for (int i = 0; i < 200 && !isDaemonRunning(); ++i) QThread::msleep(25);
    return isDaemonRunning();
}

QJsonObject probeEnvironment(const Backend &b)
{
    QJsonObject state = b.status();
    if (isDaemonRunning()) {
        QString liveJson;
        if (notifyDaemon("status", &liveJson)) {
            const auto live = QJsonDocument::fromJson(liveJson.toUtf8()).object();
            for (auto it = live.begin(); it != live.end(); ++it)
                state[it.key()] = it.value();
        }
    }
    QJsonObject programs;
    for (auto *tool : {"python3", "parec", "pw-record", "arecord", "ffmpeg",
                       "paplay", "pw-play", "aplay",
                       "wl-copy", "wl-paste", "wtype", "ydotool", "dotool",
                       "hyprctl", "swaymsg", "niri", "xdotool", "xclip", "xsel",
                       "pactl", "notify-send", "nvidia-smi"}) {
        programs[tool] = QStandardPaths::findExecutable(tool);
    }
    state["programs"] = programs;
    state["desktop"] = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    state["session"] = qEnvironmentVariable("XDG_SESSION_TYPE");
#ifdef SOL_LAYER_SHELL
    state["layerShellCompiled"] = true;
#else
    state["layerShellCompiled"] = false;
#endif
    return state;
}

QString resolveSettingKey(const QString &alias)
{
    const QString low = alias.trimmed().toLower();
    if (low == "whisper" || low == "whispermode") return "whisperMode";
    if (low == "sound" || low == "sounds" || low == "soundeffects" || low == "chimes") return "soundEffects";
    if (low == "media" || low == "pausemedia" || low == "mutemedia") return "pauseMedia";
    if (low == "clipboard" || low == "restoreclipboard") return "restoreClipboard";
    if (low == "doubletap" || low == "doubletaplock") return "doubleTapLock";
    if (low == "hud" || low == "showhud" || low == "flowbar") return "showHud";
    if (low == "theme" || low == "hudtheme" || low == "design") return "hudTheme";
    if (low == "code" || low == "codemode") return "codeMode";
    if (low == "filetag" || low == "filetagging") return "fileTagging";
    if (low == "autoinsert" || low == "insert" || low == "paste") return "autoInsert";
    if (low == "context" || low == "contextaware") return "contextAware";
    if (low == "history" || low == "keephistory") return "keepHistory";
    if (low == "mic" || low == "microphone") return "microphone";
    if (low == "lang" || low == "language") return "language";
    if (low == "langs" || low == "languages" || low == "preferredlanguages") return "preferredLanguages";
    if (low == "key" || low == "hold" || low == "shortcut" || low == "x11hold") return "shortcut";
    return alias;
}

QVariant parseCliValue(const QString &key, const QString &raw)
{
    static const QStringList boolKeys = {
        "keepHistory", "contextAware", "autoInsert", "whisperMode", "soundEffects",
        "pauseMedia", "restoreClipboard", "doubleTapLock", "fileTagging", "autostart", "showHud",
    };
    if (boolKeys.contains(key)) {
        const QString low = raw.trimmed().toLower();
        return low == "1" || low == "true" || low == "on" || low == "yes";
    }
    return raw;
}

bool copyToClipboardCli(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    auto pipe = [&](const QString &exe, const QStringList &args) {
        const QString bin = QStandardPaths::findExecutable(exe);
        if (bin.isEmpty()) return false;
        QProcess p;
        p.start(bin, args);
        if (!p.waitForStarted(800)) return false;
        p.write(bytes);
        p.closeWriteChannel();
        return p.waitForFinished(1500) && p.exitCode() == 0;
    };
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && pipe("wl-copy", {}))
        return true;
    if (pipe("xclip", {"-selection", "clipboard", "-in"})) return true;
    if (pipe("xsel", {"-ib"})) return true;
    return false;
}

int runUninstall(const QStringList &args, QTextStream &out, QTextStream &err)
{
    const bool purge = args.contains("--purge");
    const bool yes = args.contains("--yes") || args.contains("-y");

    const QString home = QDir::homePath();
    const QString prefix = qEnvironmentVariable("SOL_PREFIX", home + "/.local");
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME", home + "/.local/share");
    const QString stateHome = qEnvironmentVariable("XDG_STATE_HOME", home + "/.local/state");
    const QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config");

    CliUi::printBanner(out, "uninstall");
    out << "\n";

    QStringList filesToRemove = {
        prefix + "/bin/sol",
        prefix + "/bin/sol-setup-speech",
        prefix + "/bin/sol-setup-polish",
        dataHome + "/applications/org.sol.Dictation.desktop",
        dataHome + "/applications/sol.desktop",
        dataHome + "/icons/hicolor/scalable/apps/org.sol.Dictation.svg",
        dataHome + "/icons/hicolor/scalable/apps/sol.svg",
        configHome + "/autostart/org.sol.Dictation.desktop",
    };
    QStringList dirsToRemove = {
        prefix + "/libexec/sol",
        prefix + "/lib/sol",
    };
    if (purge) {
        dirsToRemove << (dataHome + "/sol") << (stateHome + "/sol") << (configHome + "/sol");
    }

    if (!yes && isatty(STDIN_FILENO)) {
        out << "  " << CliUi::warn("!") << " This will stop Sol, remove managed shortcuts and user-installed files";
        if (purge) out << CliUi::bad(" AND delete all downloaded models, dictionary, snippets and history");
        out << ".\n\n  " << CliUi::strong("Continue? [y/N]") << " " << Qt::flush;
        QTextStream in(stdin);
        const QString answer = in.readLine().trimmed().toLower();
        if (answer != "y" && answer != "yes") {
            out << "\n  " << CliUi::dim("Uninstall cancelled.") << "\n\n";
            return 0;
        }
    }

    QString quitReply;
    if (notifyDaemon("quit", &quitReply)) {
        if (!quitReply.isEmpty()) {
            err << "  " << CliUi::bad("[ERR]") << " " << quitReply << "\n";
            return 1;
        }
        out << "  " << CliUi::good("+") << " Stopped background daemon.\n";
        QThread::msleep(150);
    }

    Desktop::removePersistedShortcuts(configHome);

    int removed = 0;
    for (const QString &path : filesToRemove) {
        QFileInfo fi(path);
        if (fi.exists() || fi.isSymLink()) {
            if (QFile::remove(path)) {
                out << "  " << CliUi::good("+") << " Removed " << CliUi::dim(path) << "\n";
                ++removed;
            } else {
                err << "  " << CliUi::bad("x") << " Could not remove " << path << "\n";
            }
        }
    }
    for (const QString &dirPath : dirsToRemove) {
        QDir dir(dirPath);
        if (dir.exists()) {
            if (dir.removeRecursively()) {
                out << "  " << CliUi::good("+") << " Removed " << CliUi::dim(dirPath) << "\n";
                ++removed;
            } else {
                err << "  " << CliUi::bad("x") << " Could not remove " << dirPath << "\n";
            }
        }
    }

    if (QFileInfo::exists("/usr/bin/sol")) {
        out << "\n  " << CliUi::warn("! System package detected at /usr/bin/sol") << "\n";
        if (!QStandardPaths::findExecutable("pacman").isEmpty())
            out << "    Remove system package:  " << CliUi::cmd("sudo pacman -R sol") << "\n";
        else if (!QStandardPaths::findExecutable("apt").isEmpty())
            out << "    Remove system package:  " << CliUi::cmd("sudo apt remove sol") << "\n";
        else if (!QStandardPaths::findExecutable("dnf").isEmpty())
            out << "    Remove system package:  " << CliUi::cmd("sudo dnf remove sol") << "\n";
        else
            out << "    Remove it with your system package manager.\n";
    } else if (QFileInfo::exists("/usr/local/bin/sol")) {
        out << "\n  " << CliUi::warn("! System install detected at /usr/local/bin/sol") << "\n"
            << "    Remove with:  "
            << CliUi::cmd("sudo rm -rf /usr/local/bin/sol /usr/local/bin/sol-setup-* /usr/local/libexec/sol") << "\n";
    }

    if (!purge && QDir(dataHome + "/sol").exists()) {
        out << "\n  " << CliUi::imp(">") << " Kept downloaded models and personal data in "
            << CliUi::dim(dataHome + "/sol") << "\n"
            << "    Delete those too:  " << CliUi::cmd("sol uninstall --purge") << "\n";
    }
    out << "\n  " << (removed > 0 ? CliUi::good("+ Sol uninstalled.") : CliUi::dim("No user-installed Sol files were found.")) << "\n\n";
    return 0;
}

int runDictateTest(QCoreApplication &app, QTextStream &out, QTextStream &err)
{
    Backend b(false);
    if (!b.speechReady()) {
        err << "  " << CliUi::bad("[ERR]") << " Speech model not installed. Run: " << CliUi::cmd("sol setup") << "\n";
        return 1;
    }
    CliUi::printBanner(out, "dictate");
    out << "\n  " << CliUi::imp(">") << " " << CliUi::strong("Listening...")
        << "  " << CliUi::dim("Speak into your mic, then press Enter.") << "\n" << Qt::flush;

    QElapsedTimer stopToResult;
    QSocketNotifier stdinNotif(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stdinNotif, &QSocketNotifier::activated, &app, [&] {
        char buf[128];
        if (::read(STDIN_FILENO, buf, sizeof(buf)) >= 0) {
            stdinNotif.setEnabled(false);
            out << "  " << CliUi::dim("Transcribing...") << "\n" << Qt::flush;
            stopToResult.start();
            b.toggle();
        }
    });
    QObject::connect(&b, &Backend::completed, &app, [&](const QString &text, const QString &error) {
        const qint64 ms = stopToResult.isValid() ? stopToResult.elapsed() : 0;
        if (!error.isEmpty()) {
            err << "\n  " << CliUi::bad("[ERR]") << " " << CliUi::bad(error) << "\n\n";
            app.exit(1);
            return;
        }
        const QString speed = ms > 0
            ? (ms < 600 ? CliUi::good(QString::number(ms) + " ms")
                        : ms < 1500 ? CliUi::warn(QString::number(ms) + " ms")
                                    : CliUi::bad(QString::number(ms) + " ms"))
            : QString();
        out << "\n  " << CliUi::good("+") << " " << CliUi::strong("Result")
            << (speed.isEmpty() ? "" : "  " + CliUi::dim("(") + speed + CliUi::dim(")")) << "\n\n"
            << "    " << CliUi::strong(text) << "\n\n";
        if (copyToClipboardCli(text))
            out << "  " << CliUi::dim("Copied to clipboard.") << "\n\n";
        app.exit(0);
    });
    QTimer::singleShot(0, &app, [&] { b.toggle(); });
    return app.exec();
}

int runCli(QCoreApplication &app, const QStringList &args)
{
    QTextStream out(stdout), err(stderr);

    if (args.isEmpty()) {
        const bool started = startDaemonSilent(app, err);
        QJsonObject s;
        if (started) {
            QString statusJson;
            notifyDaemon("status", &statusJson);
            s = QJsonDocument::fromJson(statusJson.toUtf8()).object();
        } else {
            Backend b(false);
            s = b.status();
        }
        CliUi::printStatusCard(out, s, started);
        return started ? 0 : 1;
    }

    const QString action = args.value(0);
    if (action == "-h" || action == "--help" || action == "help") {
        CliUi::printHelp(out);
        return 0;
    }
    if (action == "--version" || action == "-v") {
        out << "sol " << app.applicationVersion() << "\n";
        return 0;
    }
    if (action == "uninstall") {
        return runUninstall(args.mid(1), out, err);
    }
    if (action == "logs") {
        const QString logFile = daemonLogPath();
        if (!QFileInfo::exists(logFile)) {
            out << "  " << CliUi::dim("No daemon log yet at " + logFile) << "\n";
            return 0;
        }
        if (args.contains("-f") || args.contains("--follow")) {
            QProcess tail;
            tail.setProcessChannelMode(QProcess::ForwardedChannels);
            tail.start("tail", {"-n", "50", "-f", logFile});
            tail.waitForFinished(-1);
            return tail.exitCode();
        }
        QFile f(logFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
            const int start = std::max(0, static_cast<int>(lines.size()) - 60);
            for (int i = start; i < lines.size(); ++i) {
                const QString &l = lines[i];
                if (l.isEmpty()) continue;
                if (l.contains("error", Qt::CaseInsensitive) || l.contains("fail", Qt::CaseInsensitive))
                    out << CliUi::bad(l) << "\n";
                else if (l.contains("warn", Qt::CaseInsensitive))
                    out << CliUi::warn(l) << "\n";
                else
                    out << CliUi::dim(l) << "\n";
            }
        }
        return 0;
    }
    if (action == "setup" || action == "setup-speech") {
        const bool skipShortcuts = args.contains("--no-shortcuts");
        const bool nonInteractive = args.contains("--yes") || args.contains("-y") || !isatty(STDIN_FILENO);
        QStringList scriptArgs;
        for (const QString &a : args.mid(1))
            if (!a.startsWith("--") && a != "-y") scriptArgs << a;

        CliUi::printBanner(out, "setup");
        CliUi::printSection(out, "Step 1: Speech Model");

        const QString script = findScript(app, "sol-setup-speech", "setup-speech");
        if (script.isEmpty()) {
            err << "  " << CliUi::bad("[ERR]") << " Could not find setup-speech script. Run " << CliUi::cmd("bin/setup-speech") << "\n";
            return 1;
        }
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedChannels);
        p.setInputChannelMode(QProcess::ForwardedInputChannel);
        p.start(script, scriptArgs);
        p.waitForFinished(-1);
        if (p.exitCode() != 0) {
            err << "\n  " << CliUi::bad("[ERR] Speech model setup failed.") << "\n\n";
            return p.exitCode();
        }
        out << "\n  " << CliUi::good("+ Speech model ready.") << "\n";

        Backend b(false);
        Desktop d;
        auto reload = [] { notifyDaemon("reload"); };
        auto portalBind = [&] {
            if (startDaemonSilent(app, err)) notifyDaemon("configure-shortcuts");
        };

        if (!nonInteractive) {
            CliUi::interactiveLanguageSetup(b, out, reload);
            CliUi::interactiveHudThemeSetup(b, out, reload);
            if (!skipShortcuts)
                CliUi::interactiveShortcutSetup(b, d, out, reload, portalBind);

            if (!b.autostart()) {
                out << "  " << CliUi::strong("Start Sol automatically at login?") << " " << CliUi::dim("[Y/n]") << ": " << Qt::flush;
                QTextStream in(stdin);
                const QString ans = in.readLine().trimmed().toLower();
                if (ans.isEmpty() || ans.startsWith('y')) {
                    b.setOption("autostart", true);
                    out << "  " << CliUi::good("+") << " Autostart enabled.\n";
                }
            }
        }

        if (QDBusConnection::sessionBus().isConnected()) {
            if (isDaemonRunning()) notifyDaemon("reload");
            else startDaemonSilent(app, err);
        }

        out << "\n  " << CliUi::good("+ Sol is ready.") << "  "
            << CliUi::dim("Hold") << " " << CliUi::key(b.shortcut()) << " "
            << CliUi::dim("in any app to speak and paste.") << "\n"
            << "  " << CliUi::dim("Languages:") << " " << CliUi::imp(b.languagesDisplay())
            << CliUi::dim("  |  ") << CliUi::cmd("sol settings")
            << CliUi::dim("  |  ") << CliUi::cmd("sol doctor") << "\n\n";
        return 0;
    }
    if (action == "shortcuts" || action == "setup-shortcuts") {
        Backend b(false);
        Desktop d;
        const QString sub = args.value(1, "show").toLower();
        auto reload = [] { notifyDaemon("reload"); };
        auto portalBind = [&] {
            if (startDaemonSilent(app, err)) notifyDaemon("configure-shortcuts");
        };

        if (sub == "setup" || sub == "choose" || sub == "configure") {
            CliUi::printBanner(out, "shortcuts");
            CliUi::interactiveShortcutSetup(b, d, out, reload, portalBind);
            return 0;
        }
        if (sub == "bind") {
            if (!startDaemonSilent(app, err)) return 1;
            notifyDaemon("configure-shortcuts");
            out << "  " << CliUi::good("+") << " Opened desktop shortcut configuration.\n";
            return 0;
        }
        if (sub == "apply") {
            const bool glassBlur = b.showHud() && b.hudTheme() == "glass";
            QString msg;
            const bool live = Desktop::applyLiveShortcuts(b.shortcut(), glassBlur, &msg);
            out << "  " << (live ? CliUi::good("+") : CliUi::imp(">")) << " " << msg << "\n";
            if (args.contains("--save") || args.contains("--persist")) {
                QString pmsg;
                if (Desktop::persistShortcuts(b.shortcut(), glassBlur, &pmsg))
                    out << "  " << CliUi::good("+") << " " << pmsg << "\n";
                else
                    out << "  " << CliUi::warn("!") << " " << pmsg << "\n";
            }
            notifyDaemon("reload");
            return 0;
        }
        if (sub == "set" && args.size() >= 3) {
            const QString chord = args.mid(2).join('+');
            b.setOption("shortcut", chord);
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Shortcut -> " << CliUi::key(chord) << "\n";
            QString msg;
            if (Desktop::applyLiveShortcuts(b.shortcut(), b.showHud() && b.hudTheme() == "glass", &msg))
                out << "  " << CliUi::good("+") << " " << msg << "\n";
            return 0;
        }
        CliUi::printBanner(out, "shortcuts");
        CliUi::printSection(out, "Active Shortcut (" + d.desktopEnvironment() + ")");
        CliUi::printRow(out, CliUi::imp(">"), "Dictation key", CliUi::key(b.shortcut()),
                        "hold to speak, release to paste (double-tap locks)");

        CliUi::printSection(out, "Desktop Config");
        for (const QString &line : d.shortcutSnippet(b.shortcut()).split('\n'))
            out << "  " << (line.startsWith('#') || line.startsWith("//") || line.startsWith("--") ? CliUi::dim(line) : CliUi::key(line)) << "\n";
        out << "\n  " << CliUi::dim("Change:") << " " << CliUi::cmd("sol shortcuts setup")
            << "   " << CliUi::dim("Apply:") << " " << CliUi::cmd("sol shortcuts apply --save") << "\n\n";
        return 0;
    }
    if (action == "doctor") {
        Backend b(false);
        const QJsonObject state = probeEnvironment(b);
        if (args.contains("--json"))
            out << QJsonDocument(state).toJson(QJsonDocument::Compact) << "\n";
        else
            CliUi::printDoctor(out, state);
        return b.speechReady() ? 0 : 1;
    }
    if (action == "settings" || action == "config") {
        Backend b(false);
        Desktop d;
        if (args.size() == 1) {
            if (action == "settings" && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
                CliUi::interactiveSettings(b, d, out, [] { notifyDaemon("reload"); }, [&] {
                    if (startDaemonSilent(app, err)) notifyDaemon("configure-shortcuts");
                });
                return 0;
            }
            CliUi::printSettingsSnapshot(out, b);
            out << "\n  " << CliUi::dim("Example:") << " " << CliUi::cmd("sol settings languages en,hu")
                << CliUi::dim(" | ") << CliUi::cmd("sol settings whisper on") << "\n\n";
            return 0;
        }
        if (args.contains("--json")) {
            const QJsonObject cfg{
                {"language", b.language()}, {"preferredLanguages", QJsonArray::fromStringList(b.preferredLanguages())},
                {"cleanup", b.cleanup()}, {"style", b.style()},
                {"personalStyle", b.personalStyle()}, {"workStyle", b.workStyle()},
                {"emailStyle", b.emailStyle()}, {"codeStyle", b.codeStyle()}, {"otherStyle", b.otherStyle()},
                {"codeMode", b.codeMode()}, {"fileTagging", b.fileTagging()}, {"whisperMode", b.whisperMode()},
                {"microphone", b.microphone()}, {"soundEffects", b.soundEffects()}, {"pauseMedia", b.pauseMedia()},
                {"restoreClipboard", b.restoreClipboard()}, {"doubleTapLock", b.doubleTapLock()},
                {"showHud", b.showHud()}, {"hudTheme", b.hudTheme()}, {"autoInsert", b.autoInsert()},
                {"contextAware", b.contextAware()}, {"keepHistory", b.keepHistory()},
                {"autostart", b.autostart()}, {"shortcut", b.shortcut()},
            };
            out << QJsonDocument(cfg).toJson(QJsonDocument::Indented);
            return 0;
        }
        const QString keyName = resolveSettingKey(args.value(1));
        const QVariant before = b.optionValue(keyName);
        if (!before.isValid()) {
            err << "  " << CliUi::bad("[ERR]") << " Unknown setting: " << CliUi::strong(args.value(1))
                << "\n  Run " << CliUi::cmd("sol settings") << " to view all options.\n";
            return 2;
        }
        if (args.size() == 2) {
            out << (keyName == "preferredLanguages" ? b.languagesDisplay() : before.toString()) << "\n";
            return 0;
        }
        const QString rawVal = args.mid(2).join(' ').trimmed();
        const QString lowVal = rawVal.toLower();
        // Support "sol settings hud glass" / "sol settings hud mono" shorthand cleanly.
        if (keyName == "showHud" && (lowVal == "glass" || lowVal == "mono")) {
            b.setOption("showHud", true);
            b.setOption("hudTheme", lowVal);
            if (Desktop::hasActiveHyprlandBinds())
                Desktop::applyLiveShortcuts(b.shortcut(), lowVal == "glass");
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " hud -> " << CliUi::imp(lowVal) << "\n";
            return 0;
        }
        b.setOption(keyName, parseCliValue(keyName, rawVal));
        if ((keyName == "hudTheme" || keyName == "showHud") && Desktop::hasActiveHyprlandBinds())
            Desktop::applyLiveShortcuts(b.shortcut(), b.showHud() && b.hudTheme() == "glass");
        notifyDaemon("reload");
        const QString updated = (keyName == "preferredLanguages" || keyName == "language")
            ? b.languagesDisplay() : b.optionValue(keyName).toString();
        out << "  " << CliUi::good("+") << " " << CliUi::strong(keyName) << " -> " << CliUi::imp(updated) << "\n";
        return 0;
    }
    if (action == "style") {
        Backend b(false);
        const QString sub = args.value(1, "show").toLower();
        if (sub == "show" || sub == "list") {
            CliUi::printStyleOverview(out, b);
            return 0;
        }
        if (QStringList{"auto", "formal", "casual", "lowercase", "excited", "verbatim"}.contains(sub)) {
            b.setOption("style", sub);
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Writing style -> " << CliUi::imp(sub) << "\n";
            return 0;
        }
        if (sub == "cat" && args.size() >= 4) {
            const QString cat = args.value(2).toLower();
            const QString tone = args.value(3).toLower();
            QString keyName;
            if (cat == "personal") keyName = "personalStyle";
            else if (cat == "work") keyName = "workStyle";
            else if (cat == "email") keyName = "emailStyle";
            else if (cat == "code") keyName = "codeStyle";
            else if (cat == "other") keyName = "otherStyle";
            if (keyName.isEmpty()) {
                err << "  " << CliUi::bad("[ERR]") << " Category must be: personal, work, email, code, or other.\n";
                return 2;
            }
            b.setOption(keyName, tone);
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " " << CliUi::strong(cat) << " style -> " << CliUi::imp(tone) << "\n";
            return 0;
        }
        if (sub == "app" && args.size() >= 4) {
            if (!b.addAppRule(args.value(2), args.value(3).toLower())) {
                err << "  " << CliUi::bad("[ERR]") << " Invalid app rule. Style must be formal|casual|lowercase|excited|verbatim.\n";
                return 2;
            }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " App rule: " << CliUi::strong(args.value(2)) << " -> " << CliUi::imp(args.value(3)) << "\n";
            return 0;
        }
        if ((sub == "remove-app" || sub == "rm-app") && args.size() >= 3) {
            if (!b.removeAppRule(args.value(2))) {
                err << "  " << CliUi::bad("[ERR]") << " App rule not found: " << args.value(2) << "\n";
                return 1;
            }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Removed app rule: " << args.value(2) << "\n";
            return 0;
        }
        err << "  Usage: " << CliUi::cmd("sol style [show | auto|formal|casual|lowercase|excited|verbatim | cat <cat> <tone> | app <name> <tone>]") << "\n";
        return 2;
    }
    if (action == "mic" || action == "microphone") {
        Backend b(false);
        b.refreshMicrophones();
        const QString sub = args.value(1, "list").toLower();
        if (sub == "test") return CliUi::runMicTest(b.microphone(), out, err);
        const auto mics = b.microphones();
        if (sub == "list") {
            CliUi::printBanner(out, "microphones");
            out << "\n";
            for (int i = 0; i < mics.size(); ++i) {
                const auto m = mics[i].toMap();
                const bool active = m["id"].toString() == b.microphone();
                out << "  " << CliUi::imp(QString("[%1]").arg(i + 1)) << " "
                    << (active ? CliUi::good("* ") : "  ")
                    << CliUi::strong(m["label"].toString()) << "  " << CliUi::dim(m["id"].toString()) << "\n";
            }
            out << "\n  " << CliUi::dim("Select:") << " " << CliUi::cmd("sol mic set 1")
                << "   " << CliUi::dim("Test:") << " " << CliUi::cmd("sol mic test") << "\n\n";
            return 0;
        }
        if (sub == "set" && args.size() >= 3) {
            const QString arg = args.value(2);
            bool okInt = false;
            const int idx = arg.toInt(&okInt);
            QString target = arg;
            if (okInt && idx >= 1 && idx <= mics.size())
                target = mics[idx - 1].toMap()["id"].toString();
            b.setOption("microphone", target);
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Microphone -> " << CliUi::imp(target) << "\n";
            return 0;
        }
        err << "  Usage: " << CliUi::cmd("sol mic [list | set <#|id> | test]") << "\n";
        return 2;
    }
    if (action == "dict" || action == "dictionary" || action == "snippet" || action == "snippets") {
        Backend b(false);
        const bool isDict = action.startsWith("dict");
        const QString kind = isDict ? "dictionary" : "snippets";
        const QString sub = args.value(1, "list").toLower();
        if (sub == "list") {
            const auto rows = isDict ? b.dictionary() : b.snippets();
            if (args.contains("--json"))
                out << QJsonDocument::fromVariant(rows).toJson(QJsonDocument::Compact) << "\n";
            else
                CliUi::printEntries(out, kind, rows);
            return 0;
        }
        if (sub == "add" && args.size() >= 3) {
            const QString trig = args.value(2);
            const QString val = args.size() >= 4 ? args.mid(3).join(' ') : trig;
            if (!b.addEntry(kind, trig, val)) {
                err << "  " << CliUi::bad("[ERR]") << " " << b.message() << "\n";
                return 1;
            }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Saved: " << CliUi::imp(trig) << " -> " << CliUi::strong(val) << "\n";
            return 0;
        }
        if ((sub == "remove" || sub == "rm" || sub == "delete") && args.size() >= 3) {
            const QString trig = args.mid(2).join(' ');
            if (!b.removeEntry(kind, trig)) {
                err << "  " << CliUi::bad("[ERR]") << " Entry not found: " << trig << "\n";
                return 1;
            }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Removed: " << trig << "\n";
            return 0;
        }
        err << "  Usage: " << CliUi::cmd(QString("sol %1 [list | add TRIGGER [VALUE] | remove TRIGGER]").arg(isDict ? "dict" : "snippet")) << "\n";
        return 2;
    }
    if (action == "stats") {
        Backend b(false);
        if (args.contains("--json")) {
            const QJsonObject obj{
                {"streakDays", b.streakDays()}, {"totalWords", b.totalWords()},
                {"totalDictations", b.totalDictations()}, {"averageWpm", b.averageWpm()},
                {"minutesSaved", b.minutesSaved()},
            };
            out << QJsonDocument(obj).toJson(QJsonDocument::Compact) << "\n";
        } else {
            CliUi::printStats(out, b);
        }
        return 0;
    }
    if (action == "history") {
        Backend b(false);
        const QString sub = args.value(1, "list").toLower();
        const auto rows = b.history();
        if (sub == "clear") {
            b.clearHistory();
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " History cleared.\n";
            return 0;
        }
        if (sub == "copy" && args.size() >= 3) {
            const int idx = args.value(2).toInt() - 1;
            if (idx < 0 || idx >= rows.size()) { err << "  " << CliUi::bad("[ERR]") << " Invalid index.\n"; return 1; }
            const QString txt = rows[idx].toMap()["text"].toString();
            if (copyToClipboardCli(txt)) out << "  " << CliUi::good("+") << " Copied #" << (idx + 1) << " to clipboard.\n";
            else out << txt << "\n";
            return 0;
        }
        if (sub == "raw" && args.size() >= 3) {
            const int idx = args.value(2).toInt() - 1;
            if (idx < 0 || idx >= rows.size()) { err << "  " << CliUi::bad("[ERR]") << " Invalid index.\n"; return 1; }
            if (!b.revertToRaw(rows[idx].toMap()["id"].toString())) { err << "  " << CliUi::bad("[ERR]") << " No raw text available.\n"; return 1; }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Reverted #" << (idx + 1) << " to raw transcription.\n";
            return 0;
        }
        if (sub == "edit" && args.size() >= 4) {
            const int idx = args.value(2).toInt() - 1;
            if (idx < 0 || idx >= rows.size()) { err << "  " << CliUi::bad("[ERR]") << " Invalid index.\n"; return 1; }
            if (!b.editHistory(rows[idx].toMap()["id"].toString(), args.mid(3).join(' '))) { err << "  " << CliUi::bad("[ERR]") << " Could not update entry.\n"; return 1; }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " " << b.message() << "\n";
            return 0;
        }
        if (sub == "reformat" && args.size() >= 4) {
            const int idx = args.value(2).toInt() - 1;
            if (idx < 0 || idx >= rows.size()) { err << "  " << CliUi::bad("[ERR]") << " Invalid index.\n"; return 1; }
            if (!b.reformatHistory(rows[idx].toMap()["id"].toString(), args.value(3).toLower())) { err << "  " << CliUi::bad("[ERR]") << " Could not reformat.\n"; return 1; }
            notifyDaemon("reload");
            out << "  " << CliUi::good("+") << " Reformatted #" << (idx + 1) << " as " << CliUi::imp(args.value(3)) << ".\n";
            return 0;
        }
        int limit = 15;
        const int limIdx = args.indexOf("--limit");
        if (limIdx >= 0 && limIdx + 1 < args.size()) limit = std::max(1, args.value(limIdx + 1).toInt());
        QString search;
        const int srchIdx = args.indexOf("--search");
        if (srchIdx >= 0 && srchIdx + 1 < args.size()) search = args.value(srchIdx + 1);
        if (args.contains("--json")) {
            auto subset = rows.size() > limit ? rows.mid(0, limit) : rows;
            out << QJsonDocument::fromVariant(subset).toJson(QJsonDocument::Compact) << "\n";
        } else {
            CliUi::printHistory(out, rows, limit, search);
        }
        return 0;
    }
    if (action == "dictate") {
        return runDictateTest(app, out, err);
    }
    if (action == "portal-input") {
        if (!startDaemonSilent(app, err)) return 1;
        notifyDaemon("configure-input");
        out << "  " << CliUi::good("+") << " Requested desktop keyboard and clipboard access via XDG portal.\n";
        return 0;
    }
    if (action == "transcribe") {
        QCommandLineParser parser;
        parser.addOption({"json", "Return structured transcription."});
        parser.addOption({"language", "Language code or auto.", "code", ""});
        parser.addOption({"style", "Writing style (formal, casual, lowercase, excited, verbatim).", "style", ""});
        parser.addOption({"cleanup", "Cleanup level (none, light, medium).", "mode", ""});
        parser.addPositionalArgument("audio", "Audio file.");
        if (!parser.parse(args) || parser.positionalArguments().size() != 1) {
            CliUi::printHelp(err);
            return 2;
        }
        const auto path = QFileInfo(parser.positionalArguments().first()).absoluteFilePath();
        Backend backend(false);
        QObject::connect(&backend, &Backend::completed, &app, [&](const QString &text, const QString &error) {
            if (!error.isEmpty()) {
                err << CliUi::bad("[ERR]") << " " << error << "\n";
            } else if (parser.isSet("json")) {
                out << QJsonDocument(QJsonObject{{"text", text}, {"notice", backend.message()}})
                           .toJson(QJsonDocument::Compact) << "\n";
            } else {
                out << text << "\n";
                if (!backend.message().isEmpty()) err << CliUi::warn("[WARN]") << " " << backend.message() << "\n";
            }
            app.exit(error.isEmpty() ? 0 : 1);
        });
        QTimer::singleShot(0, &app, [&] {
            backend.transcribe(path, parser.value("language"), parser.value("style"), parser.value("cleanup"));
        });
        return app.exec();
    }

    static const QStringList daemonActions = {
        "start", "stop", "toggle", "cancel", "paste-last", "retry", "status", "quit", "reload",
    };
    if (!daemonActions.contains(action)) {
        err << "  " << CliUi::bad("[ERR]") << " Unknown command: " << CliUi::strong(action) << "\n";
        CliUi::printHelp(err);
        return 2;
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        err << "  " << CliUi::bad("[ERR]") << " Session D-Bus is required for daemon controls.\n";
        return 1;
    }
    if (!isDaemonRunning()) {
        if (action == "quit" || action == "stop" || action == "cancel" || action == "reload")
            return 0;
        if (action == "status") {
            if (args.contains("--json")) {
                out << "{\"state\":\"stopped\"}\n";
            } else {
                Backend b(false);
                CliUi::printStatusCard(out, b.status(), false);
            }
            return 0;
        }
        if (!startDaemonSilent(app, err)) return 1;
    }

    QString reply;
    if (!notifyDaemon(action, &reply)) {
        err << "  " << CliUi::bad("[ERR]") << " Could not reach Sol daemon.\n";
        return 1;
    }
    if (action == "status") {
        if (args.contains("--json"))
            out << reply << "\n";
        else
            CliUi::printStatusCard(out, QJsonDocument::fromJson(reply.toUtf8()).object(), true);
    } else if (action == "quit") {
        if (!reply.isEmpty()) { err << "  " << CliUi::bad("[ERR]") << " " << reply << "\n"; return 1; }
        out << "  " << CliUi::good("+") << " Sol stopped.\n";
    } else if (!reply.isEmpty()) {
        err << "  " << CliUi::bad("[ERR]") << " " << reply << "\n";
        return 1;
    }
    return 0;
}

void positionFloatingOverlay(QWindow *win)
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect avail = screen->availableGeometry();
    const int x = avail.x() + (avail.width() - win->width()) / 2;
    const int y = avail.y() + avail.height() - win->height() - 36;
    win->setPosition(x, y);
}
} // namespace

int main(int argc, char **argv)
{
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const QString action = args.value(0);

    // Every non-daemon CLI invocation stays 100% headless with zero GUI display overhead.
    if (action != "daemon" && action != "_hud") {
        QCoreApplication app(argc, argv);
        metadata();
        return runCli(app, args);
    }

    const bool useLayerShell = [&] {
#ifdef SOL_LAYER_SHELL
        return action == "_hud" && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
               && qEnvironmentVariable("QT_QPA_PLATFORM") != "xcb";
#else
        return false;
#endif
    }();
    if (useLayerShell)
        qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");

    QGuiApplication app(argc, argv);
    metadata();
    app.setDesktopFileName(service);
    app.setQuitOnLastWindowClosed(false);

    if (action == "_hud") {
        const QString hudTheme = (args.value(1) == "mono") ? QStringLiteral("mono") : QStringLiteral("glass");
        HudWindow win;
        win.setTheme(hudTheme);
        HudBridge bridge;
        QObject::connect(&win, &HudWindow::messageTriggered, &bridge, &HudBridge::sendMessage);

        if (useLayerShell) {
#ifdef SOL_LAYER_SHELL
            auto *layer = LayerShellQt::Window::get(&win);
            layer->setScope(hudTheme == "glass" ? "sol-dictation-glass" : "sol-dictation");
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setAnchors(LayerShellQt::Window::AnchorBottom);
            layer->setMargins({0, 0, 0, 44});
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layer->setExclusiveZone(-1);
            layer->setWantsToBeOnActiveScreen(true);
            QObject::connect(&win, &HudWindow::editingChanged, &win, [&win, layer](bool editing) {
                layer->setKeyboardInteractivity(editing
                    ? LayerShellQt::Window::KeyboardInteractivityExclusive
                    : LayerShellQt::Window::KeyboardInteractivityNone);
                if (editing) {
                    win.setVisible(true);
                    win.requestActivate();
                } else if (!win.shouldBeVisible()) {
                    win.setVisible(false);
                }
                win.update();
            });
#endif
        } else {
            win.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                         | Qt::WindowDoesNotAcceptFocus | Qt::BypassWindowManagerHint);
            positionFloatingOverlay(&win);
            QObject::connect(&win, &HudWindow::editingChanged, &win, [&win](bool editing) {
                if (editing) {
                    win.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
                    positionFloatingOverlay(&win);
                    win.show();
                    win.requestActivate();
                } else {
                    win.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                 | Qt::WindowDoesNotAcceptFocus | Qt::BypassWindowManagerHint);
                    if (!win.shouldBeVisible()) win.setVisible(false);
                    else { positionFloatingOverlay(&win); win.show(); }
                }
            });
        }

        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
        QByteArray buffer;
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
            char chunk[4096];
            const auto n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n <= 0) {
                app.quit();
                return;
            }
            buffer.append(chunk, n);
            qsizetype end;
            while ((end = buffer.indexOf('\n')) >= 0) {
                const auto msg = QJsonDocument::fromJson(buffer.left(end)).object();
                buffer.remove(0, end + 1);
                win.applyState(msg);
                const bool show = win.shouldBeVisible();
                // Never call setFlag() on Wayland LayerShellQt — it recreates the surface and can disturb focus.
                if (!useLayerShell && !win.isEditing()) {
                    if (show) positionFloatingOverlay(&win);
                }
                win.setVisible(show);
            }
        });
        return app.exec();
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return 1;
    if (bus.interface()->isServiceRegistered(service).value()) return 0;

    // Silent background daemon: warm speech worker + floating HUD pill when dictating.
    Backend backend(true);
    Control ctl;
    ctl.backend = &backend;
    if (!bus.registerObject(object, &ctl, QDBusConnection::ExportAllSlots) || !bus.registerService(service))
        return 0;
    return app.exec();
}
#include "main.moc"
