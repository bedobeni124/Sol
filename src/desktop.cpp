#include "desktop.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <cstring>
#include <utility>

namespace {
constexpr auto kMarkerStart = "# >>> sol shortcuts >>>";
constexpr auto kMarkerEnd = "# <<< sol shortcuts <<<";
constexpr auto kLuaMarkerStart = "-- >>> sol shortcuts >>>";
constexpr auto kLuaMarkerEnd = "-- <<< sol shortcuts <<<";

QString code(const QString &name)
{
    QFile f(":/integration/" + name + ".py");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString python()
{
    // Distribution PyGObject and AT-SPI bindings belong to the system interpreter.
    return QFile::exists("/usr/bin/python3") ? "/usr/bin/python3" : QStandardPaths::findExecutable("python3");
}

bool isGnomeLike()
{
    const QString de = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    return de.contains("gnome") || de.contains("ubuntu") || de.contains("pantheon") || de.contains("budgie");
}

#ifdef SOL_LAYER_SHELL
bool isWlrootsOrKdeWayland()
{
    if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE")
        || qEnvironmentVariableIsSet("SWAYSOCK")
        || qEnvironmentVariableIsSet("NIRI_SOCKET")
        || qEnvironmentVariableIsSet("RIVER_DISPLAY"))
        return true;
    const QString de = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    return de.contains("kde") || de.contains("plasma") || de.contains("hyprland")
           || de.contains("sway") || de.contains("niri") || de.contains("river")
           || de.contains("wayfire") || de.contains("labwc") || de.contains("cosmic")
           || de.contains("wlroots");
}
#endif

struct ParsedChord {
    QStringList mods; // canonical lowercase: super, ctrl, alt, shift
    QString key;
};

ParsedChord parseChord(const QString &chord)
{
    ParsedChord out;
    const QStringList parts = chord.split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return out;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString p = parts[i].trimmed().toLower();
        if (p == "super" || p == "meta" || p == "logo" || p == "win" || p == "mod" || p == "mod4") out.mods << "super";
        else if (p == "ctrl" || p == "control") out.mods << "ctrl";
        else if (p == "alt" || p == "option" || p == "mod1") out.mods << "alt";
        else if (p == "shift") out.mods << "shift";
    }
    out.mods.removeDuplicates();
    out.key = parts.last().trimmed();
    return out;
}

QString stripMarkerBlock(const QString &content, const char *mStart, const char *mEnd)
{
    const qsizetype start = content.indexOf(mStart);
    if (start < 0) return content;
    const qsizetype end = content.indexOf(mEnd, start);
    if (end < 0) return content;
    qsizetype after = end + static_cast<qsizetype>(std::strlen(mEnd));
    if (after < content.size() && content[after] == '\n') ++after;
    return content.left(start) + content.mid(after);
}

bool upsertMarkerBlock(const QString &filePath, const QString &block,
                       const char *mStart = kMarkerStart, const char *mEnd = kMarkerEnd)
{
    QString existing;
    QFile readFile(filePath);
    if (readFile.open(QIODevice::ReadOnly))
        existing = QString::fromUtf8(readFile.readAll());
    QString cleaned = stripMarkerBlock(existing, mStart, mEnd);
    if (!cleaned.isEmpty() && !cleaned.endsWith('\n')) cleaned += '\n';
    cleaned += QString::fromLatin1(mStart) + "\n" + block.trimmed() + "\n"
               + QString::fromLatin1(mEnd) + "\n";
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSaveFile out(filePath);
    const QByteArray bytes = cleaned.toUtf8();
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
}

bool fileContainsMarker(const QString &filePath, const char *marker)
{
    QFile f(filePath);
    return f.open(QIODevice::ReadOnly) && f.readAll().contains(marker);
}

QString luaQuote(QString s)
{
    return '"' + s.replace('\\', "\\\\").replace('"', "\\\"") + '"';
}

bool isStandaloneModifierKey(const QString &chord)
{
    const QString k = chord.trimmed().toLower();
    return k == "alt_r" || k == "rightalt" || k == "alt_l"
           || k == "control_r" || k == "rightctrl" || k == "super_r" || k == "rightsuper";
}
} // namespace

Desktop::Desktop(QObject *parent)
    : QObject(parent)
{
}

Desktop::~Desktop()
{
    for (auto *proc : {&shortcuts, &hud, &input, &desktopWorker}) {
        if (proc->state() != QProcess::NotRunning) {
            proc->closeWriteChannel();
            if (!proc->waitForFinished(300)) {
                proc->kill();
                proc->waitForFinished(800);
            }
        }
    }
}

QString Desktop::toHyprChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QStringList hm;
    for (const QString &m : c.mods) {
        if (m == "super") hm << "SUPER";
        else if (m == "ctrl") hm << "CTRL";
        else if (m == "alt") hm << "ALT";
        else if (m == "shift") hm << "SHIFT";
    }
    QString k = c.key;
    const QString low = k.toLower();
    if (low == "space") k = "SPACE";
    else if (low == "rightalt" || low == "alt_r") k = "Alt_R";
    else if (low == "rightctrl" || low == "control_r") k = "Control_R";
    else if (low == "rightsuper" || low == "super_r") k = "Super_R";
    else if (k.size() == 1) k = k.toUpper();
    return hm.join(' ') + ", " + k;
}

QString Desktop::toHyprLuaChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QStringList parts;
    for (const QString &m : c.mods) {
        if (m == "super") parts << "SUPER";
        else if (m == "ctrl") parts << "CTRL";
        else if (m == "alt") parts << "ALT";
        else if (m == "shift") parts << "SHIFT";
    }
    QString k = c.key;
    const QString low = k.toLower();
    if (low == "space") k = "SPACE";
    else if (low == "rightalt" || low == "alt_r") k = "Alt_R";
    else if (low == "rightctrl" || low == "control_r") k = "Control_R";
    else if (low == "rightsuper" || low == "super_r") k = "Super_R";
    else if (k.size() == 1) k = k.toUpper();
    if (parts.isEmpty() && k == "Alt_R") parts << "ALT";
    else if (parts.isEmpty() && k == "Control_R") parts << "CTRL";
    else if (parts.isEmpty() && k == "Super_R") parts << "SUPER";
    parts << k;
    return parts.join(" + ");
}

bool Desktop::isHyprlandLua(const QString &configHomeOverride)
{
    const QString cfg = configHomeOverride.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        : configHomeOverride;
    return QFileInfo::exists(cfg + "/hypr/hyprland.lua") || QFileInfo::exists(cfg + "/hypr/bindings.lua");
}

QString Desktop::hyprLuaScript(const QString &holdChord, bool enableGlassBlur)
{
    QStringList lines = {
        "-- Managed by Sol (run 'sol uninstall' to remove cleanly)",
        "local function sol_bind_hold(keys, start_cmd, stop_cmd, desc, is_mod_key)",
        "  local held = false",
        "  local press_opts = { description = desc }",
        "  if is_mod_key then press_opts.ignore_mods = true end",
        "  hl.bind(keys, function()",
        "    held = true",
        "    hl.exec_cmd(start_cmd)",
        "  end, press_opts)",
        "  hl.bind(keys, function()",
        "    if held then",
        "      held = false",
        "      hl.exec_cmd(stop_cmd)",
        "    end",
        "  end, { description = desc .. ' (release)', release = true, transparent = true, ignore_mods = true, non_consuming = true })",
        "end",
    };
    if (!holdChord.isEmpty()) {
        lines << QString("sol_bind_hold(%1, 'sol start', 'sol stop', 'Sol: Hold to dictate', %2)")
                     .arg(luaQuote(toHyprLuaChord(holdChord)),
                          isStandaloneModifierKey(holdChord) ? "true" : "false");
    }
    if (enableGlassBlur) {
        lines << "hl.config({ decoration = { blur = { enabled = true, size = 6, passes = 3, new_optimizations = true, vibrancy = 0.25 } } })";
        lines << "hl.layer_rule({ match = { namespace = '^sol-dictation-glass$' }, blur = true, ignore_alpha = 0.08 })";
    }
    return lines.join('\n') + '\n';
}

QString Desktop::toSwayChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QStringList sm;
    for (const QString &m : c.mods) {
        if (m == "super") sm << "Mod4";
        else if (m == "ctrl") sm << "Control";
        else if (m == "alt") sm << "Mod1";
        else if (m == "shift") sm << "Shift";
    }
    QString k = c.key;
    const QString low = k.toLower();
    if (low == "space") k = "space";
    else if (low == "rightalt") k = "Alt_R";
    else if (k.size() == 1) k = k.toLower();
    sm << k;
    return sm.join('+');
}

QString Desktop::toNiriChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QStringList nm;
    for (const QString &m : c.mods) {
        if (m == "super") nm << "Mod";
        else if (m == "ctrl") nm << "Ctrl";
        else if (m == "alt") nm << "Alt";
        else if (m == "shift") nm << "Shift";
    }
    QString k = c.key;
    if (k.toLower() == "space") k = "Space";
    else if (k.size() == 1) k = k.toUpper();
    nm << k;
    return nm.join('+');
}

QString Desktop::toGnomeChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QString out;
    for (const QString &m : c.mods) {
        if (m == "super") out += "<Super>";
        else if (m == "ctrl") out += "<Control>";
        else if (m == "alt") out += "<Alt>";
        else if (m == "shift") out += "<Shift>";
    }
    QString k = c.key;
    if (k.toLower() == "space") k = "space";
    else if (k.size() == 1) k = k.toLower();
    return out + k;
}

QString Desktop::toPortalChord(const QString &chord)
{
    const ParsedChord c = parseChord(chord);
    if (c.key.isEmpty()) return {};
    QStringList pm;
    for (const QString &m : c.mods) {
        if (m == "super") pm << "LOGO";
        else if (m == "ctrl") pm << "CTRL";
        else if (m == "alt") pm << "ALT";
        else if (m == "shift") pm << "SHIFT";
    }
    pm << (c.key.size() == 1 ? c.key.toLower() : c.key);
    return pm.join('+');
}

bool Desktop::hasActiveHyprlandBinds()
{
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (hyprctl.isEmpty()) return false;
    QProcess p;
    p.start(hyprctl, {"-j", "binds"});
    if (!p.waitForFinished(800) || p.exitCode() != 0) return false;
    const QByteArray out = p.readAllStandardOutput();
    return out.contains("sol start") || out.contains("sol toggle") || out.contains("Sol:");
}

bool Desktop::applyLiveShortcuts(const QString &holdChord, bool enableGlassBlur, QString *summaryOut)
{
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && !hyprctl.isEmpty()) {
        if (isHyprlandLua()) {
            const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
            const QString solLua = cfg + "/sol/shortcuts.lua";
            QDir().mkpath(cfg + "/sol");
            QSaveFile f(solLua);
            const QByteArray bytes = hyprLuaScript(holdChord, enableGlassBlur).toUtf8();
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
                if (summaryOut) *summaryOut = "Could not write " + solLua;
                return false;
            }
            const bool alreadyPersisted = fileContainsMarker(cfg + "/hypr/bindings.lua", kLuaMarkerStart)
                                          || fileContainsMarker(cfg + "/hypr/hyprland.lua", kLuaMarkerStart);
            QProcess p;
            if (alreadyPersisted)
                p.start(hyprctl, {"reload"});
            else
                p.start(hyprctl, {"eval", QString("dofile(%1)").arg(luaQuote(solLua))});
            const bool finished = p.waitForFinished(1500);
            const QString reply = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            const bool ok = finished && p.exitCode() == 0 && reply.compare("ok", Qt::CaseInsensitive) == 0;
            if (summaryOut)
                *summaryOut = ok ? "Applied live Hyprland (Lua) shortcut immediately."
                                 : QString("Hyprland Lua apply failed: %1").arg(reply);
            return ok;
        }
        auto bind = [&](const QString &kw, const QString &spec) {
            QProcess p;
            p.start(hyprctl, {"keyword", kw, spec});
            if (!p.waitForFinished(1000) || p.exitCode() != 0) return false;
            return QString::fromUtf8(p.readAllStandardOutput()).trimmed().compare("ok", Qt::CaseInsensitive) == 0;
        };
        bool ok = true;
        if (!holdChord.isEmpty()) {
            const QString hc = toHyprChord(holdChord);
            ok &= bind("bind", hc + ", exec, sol start");
            ok &= bind("bindr", hc + ", exec, sol stop");
        }
        if (enableGlassBlur) {
            bind("decoration:blur:enabled", "true");
            bind("layerrule", "blur, sol-dictation-glass");
            bind("layerrule", "ignorealpha 0.08, sol-dictation-glass");
        }
        if (summaryOut)
            *summaryOut = ok ? "Applied live Hyprland shortcut immediately."
                             : "Could not apply Hyprland shortcut via hyprctl.";
        return ok;
    }

    const QString swaymsg = QStandardPaths::findExecutable("swaymsg");
    if (!qEnvironmentVariableIsEmpty("SWAYSOCK") && !swaymsg.isEmpty()) {
        auto bind = [&](const QString &cmd) {
            QProcess p;
            p.start(swaymsg, {cmd});
            return p.waitForFinished(1000) && p.exitCode() == 0;
        };
        bool ok = true;
        if (!holdChord.isEmpty()) {
            const QString sc = toSwayChord(holdChord);
            ok &= bind("bindsym " + sc + " exec sol start");
            ok &= bind("bindsym --release " + sc + " exec sol stop");
        }
        if (summaryOut)
            *summaryOut = ok ? "Applied live Sway shortcut immediately."
                             : "Could not apply Sway shortcut via swaymsg.";
        return ok;
    }

    if (summaryOut)
        *summaryOut = "Shortcut saved for X11/portal daemon or manual desktop config.";
    return false;
}

bool Desktop::persistShortcuts(const QString &holdChord, bool enableGlassBlur, QString *summaryOut)
{
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") || QDir(cfg + "/hypr").exists()) {
        if (isHyprlandLua(cfg)) {
            const QString solLua = cfg + "/sol/shortcuts.lua";
            QDir().mkpath(cfg + "/sol");
            QSaveFile f(solLua);
            const QByteArray bytes = hyprLuaScript(holdChord, enableGlassBlur).toUtf8();
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
                if (summaryOut) *summaryOut = "Could not write " + solLua;
                return false;
            }
            const QString target = QFileInfo::exists(cfg + "/hypr/bindings.lua")
                ? cfg + "/hypr/bindings.lua"
                : cfg + "/hypr/hyprland.lua";
            const QString loader = QString("dofile(%1)").arg(luaQuote(solLua));
            if (!upsertMarkerBlock(target, loader, kLuaMarkerStart, kLuaMarkerEnd)) {
                if (summaryOut) *summaryOut = "Wrote " + solLua + " (add '" + loader + "' to " + target + ").";
                return false;
            }
            const QString legacyConf = cfg + "/hypr/hyprland.conf";
            if (QFileInfo::exists(legacyConf)) {
                QFile lf(legacyConf);
                if (lf.open(QIODevice::ReadOnly)) {
                    const QString cleaned = stripMarkerBlock(QString::fromUtf8(lf.readAll()), kMarkerStart, kMarkerEnd).trimmed();
                    lf.close();
                    if (cleaned.isEmpty()) QFile::remove(legacyConf);
                }
            }
            QFile::remove(cfg + "/sol/hypr-shortcuts.conf");
            const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
            if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && !hyprctl.isEmpty()) {
                QProcess rel;
                rel.start(hyprctl, {"reload"});
                rel.waitForFinished(1500);
            }
            if (summaryOut)
                *summaryOut = "Saved Hyprland Lua shortcut to " + solLua + " (loaded from " + target + ").";
            return true;
        }

        QStringList lines = {"# Managed by Sol (run 'sol uninstall' to remove cleanly)"};
        if (!holdChord.isEmpty()) {
            const QString hc = toHyprChord(holdChord);
            lines << ("bind  = " + hc + ", exec, sol start");
            lines << ("bindr = " + hc + ", exec, sol stop");
        }
        if (enableGlassBlur) {
            lines << "layerrule = blur, sol-dictation-glass";
            lines << "layerrule = ignorealpha 0.08, sol-dictation-glass";
        }
        const QString solConf = cfg + "/sol/hypr-shortcuts.conf";
        QDir().mkpath(cfg + "/sol");
        QSaveFile f(solConf);
        const QByteArray bytes = (lines.join('\n') + '\n').toUtf8();
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
            if (summaryOut) *summaryOut = "Could not write " + solConf;
            return false;
        }
        const QString target = QFileInfo::exists(cfg + "/hypr/bindings.conf")
            ? cfg + "/hypr/bindings.conf"
            : cfg + "/hypr/hyprland.conf";
        if (!upsertMarkerBlock(target, "source = " + solConf, kMarkerStart, kMarkerEnd)) {
            if (summaryOut) *summaryOut = "Wrote " + solConf + " (add 'source = " + solConf + "' to " + target + ").";
            return false;
        }
        if (summaryOut) *summaryOut = "Saved Hyprland shortcut to " + solConf + " (sourced from " + target + ").";
        return true;
    }

    if (!qEnvironmentVariableIsEmpty("SWAYSOCK") || QDir(cfg + "/sway").exists()) {
        QStringList lines = {"# Managed by Sol (run 'sol uninstall' to remove cleanly)"};
        if (!holdChord.isEmpty()) {
            const QString sc = toSwayChord(holdChord);
            lines << ("bindsym " + sc + " exec sol start");
            lines << ("bindsym --release " + sc + " exec sol stop");
        }
        const QString solConf = cfg + "/sol/sway-shortcuts.conf";
        QDir().mkpath(cfg + "/sol");
        QSaveFile f(solConf);
        const QByteArray bytes = (lines.join('\n') + '\n').toUtf8();
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) return false;
        const QString target = cfg + "/sway/config";
        if (QFileInfo::exists(target))
            upsertMarkerBlock(target, "include " + solConf, kMarkerStart, kMarkerEnd);
        if (summaryOut) *summaryOut = "Saved Sway shortcut to " + solConf;
        return true;
    }

    if (summaryOut) *summaryOut = "Shortcut saved in Sol configuration.";
    return true;
}

void Desktop::removePersistedShortcuts(const QString &configHomeOverride)
{
    const QString cfg = configHomeOverride.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        : configHomeOverride;
    auto cleanFile = [](const QString &target, const char *mStart, const char *mEnd, bool removeIfEmpty = false) {
        QFile f(target);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        if (!content.contains(mStart)) return;
        const QString cleaned = stripMarkerBlock(content, mStart, mEnd);
        if (removeIfEmpty && cleaned.trimmed().isEmpty()) {
            QFile::remove(target);
            return;
        }
        QSaveFile out(target);
        const QByteArray bytes = cleaned.toUtf8();
        if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size())
            out.commit();
    };
    const bool hasLua = isHyprlandLua(cfg);
    cleanFile(cfg + "/hypr/bindings.lua", kLuaMarkerStart, kLuaMarkerEnd);
    cleanFile(cfg + "/hypr/hyprland.lua", kLuaMarkerStart, kLuaMarkerEnd);
    cleanFile(cfg + "/hypr/bindings.conf", kMarkerStart, kMarkerEnd);
    cleanFile(cfg + "/hypr/hyprland.conf", kMarkerStart, kMarkerEnd, hasLua);
    cleanFile(cfg + "/sway/config", kMarkerStart, kMarkerEnd);
    QFile::remove(cfg + "/sol/shortcuts.lua");
    QFile::remove(cfg + "/sol/hypr-shortcuts.conf");
    QFile::remove(cfg + "/sol/sway-shortcuts.conf");
}

QString Desktop::desktopEnvironment() const
{
    if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE")) return "Hyprland";
    if (qEnvironmentVariableIsSet("SWAYSOCK")) return "Sway";
    if (qEnvironmentVariableIsSet("NIRI_SOCKET")) return "Niri";
    const QString de = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (!de.isEmpty()) return de;
    if (waylandSession) return "Wayland";
    if (x11Session) return "X11";
    return "Linux";
}

QString Desktop::shortcutSnippet(const QString &holdChord) const
{
    const QString de = desktopEnvironment().toLower();
    if (de.contains("hyprland")) {
        if (isHyprlandLua())
            return hyprLuaScript(holdChord).trimmed();
        return QStringLiteral(
            "# ~/.config/hypr/hyprland.conf\n"
            "bind  = %1, exec, sol start\n"
            "bindr = %1, exec, sol stop")
            .arg(toHyprChord(holdChord));
    }
    if (de.contains("sway") || de.contains("i3")) {
        return QStringLiteral(
            "# ~/.config/sway/config (or ~/.config/i3/config)\n"
            "bindsym %1 exec sol start\n"
            "bindsym --release %1 exec sol stop")
            .arg(toSwayChord(holdChord));
    }
    if (de.contains("niri")) {
        return QStringLiteral(
            "// ~/.config/niri/config.kdl\n"
            "%1 { spawn \"sol\" \"toggle\"; }")
            .arg(toNiriChord(holdChord));
    }
    return QStringLiteral(
        "# Bind in your desktop Keyboard Shortcuts:\n"
        "# Hold to dictate:  press -> sol start | release -> sol stop  (%1)\n"
        "# Or single tap:    sol toggle")
        .arg(holdChord);
}

void Desktop::initialize(const QString &holdChord)
{
    waylandSession = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    x11Session = !waylandSession && !qEnvironmentVariableIsEmpty("DISPLAY");

    const bool hypr = qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE");
    nativeKeys = hypr && hasActiveHyprlandBinds();

#ifdef SOL_LAYER_SHELL
    if (waylandSession && isWlrootsOrKdeWayland()) {
        hudAvailable = true;
        hudUseXcb = false;
    } else
#endif
    if (x11Session) {
        hudAvailable = true;
        hudUseXcb = false;
    } else if (waylandSession && !qEnvironmentVariableIsEmpty("DISPLAY") && isGnomeLike()) {
        hudAvailable = true;
        hudUseXcb = true;
    }

    if (scripts.isValid()) {
        for (auto name : {"desktop", "portalinput"}) {
            QString file = QString::fromLatin1(name) + ".py";
            QFile::copy(":/integration/" + file, scripts.filePath(file));
        }
    }

    ensureDesktopWorker();

    const bool genericWayland = waylandSession
        && !qEnvironmentVariableIsSet("SWAYSOCK")
        && !hypr
        && !qEnvironmentVariableIsSet("NIRI_SOCKET");
    if (genericWayland && scripts.isValid()) {
        inputInfo = tr("Accessibility input active. Run 'sol portal-input' for terminals/Electron.");
        connect(&input, &QProcess::readyReadStandardOutput, this, [this] {
            while (input.canReadLine()) {
                auto msg = QJsonDocument::fromJson(input.readLine()).object();
                if (msg.contains("id")) {
                    auto done = deliveries.take(msg["id"].toInt());
                    if (done) done(msg);
                } else if (msg.contains("ready")) {
                    portalReady = msg["ready"].toBool();
                    inputInfo = msg["info"].toString();
                    emit shortcutsChanged();
                }
            }
        });
        connect(&input, &QProcess::finished, this, [this] {
            portalReady = false;
            inputInfo = tr("Keyboard access ended. Run 'sol portal-input' to reconnect.");
            auto pending = std::exchange(deliveries, {});
            for (const auto &done : pending)
                done({{"notice", tr("Desktop keyboard access ended. Run 'sol history' to copy.")}});
            emit shortcutsChanged();
        });
        input.start(python(), {"-u", scripts.filePath("portalinput.py")});
    } else {
        inputInfo = tr("%1 native paste with automatic clipboard restore.").arg(desktopEnvironment());
    }

    if (nativeKeys) {
        hold = holdChord.isEmpty() ? "F9" : holdChord;
        info = tr("Hyprland shortcut active.");
        return;
    }

    info = tr("Run 'sol shortcuts setup' to bind your dictation key.");
    connect(&shortcuts, &QProcess::readyReadStandardOutput, this, [this] {
        while (shortcuts.canReadLine()) {
            const auto msg = QJsonDocument::fromJson(shortcuts.readLine()).object();
            if (msg.contains("info")) info = msg["info"].toString();
            if (msg.contains("hold")) hold = msg["hold"].toString();
            if (msg.contains("action")) emit action(msg["action"].toString());
            else emit shortcutsChanged();
        }
    });
    auto env = QProcessEnvironment::systemEnvironment();
    if (!holdChord.isEmpty()) {
        env.insert("SOL_HOLD_KEY", holdChord);
        env.insert("SOL_PORTAL_KEY", toPortalChord(holdChord));
    }
    shortcuts.setProcessEnvironment(env);
    shortcuts.start(python(), {"-u", "-c", code("shortcuts")});
}

void Desktop::ensureDesktopWorker()
{
    if (desktopWorker.state() != QProcess::NotRunning || !scripts.isValid()) return;
    desktopWorkerReady = false;
    desktopWorker.disconnect(this);
    connect(&desktopWorker, &QProcess::readyReadStandardOutput, this, [this] {
        while (desktopWorker.canReadLine()) {
            const auto msg = QJsonDocument::fromJson(desktopWorker.readLine()).object();
            if (msg.value("ready").toBool()) {
                desktopWorkerReady = true;
                const auto queued = std::exchange(workerQueue, {});
                for (const auto &entry : queued) {
                    if (workerJobs.contains(entry.first))
                        desktopWorker.write(QJsonDocument(entry.second).toJson(QJsonDocument::Compact) + '\n');
                }
            } else if (msg.contains("id")) {
                handleWorkerResponse(msg);
            }
        }
    });
    connect(&desktopWorker, &QProcess::finished, this, [this] {
        desktopWorkerReady = false;
        const auto stale = std::exchange(workerJobs, {});
        workerQueue.clear();
        for (auto it = stale.begin(); it != stale.end(); ++it) {
            if (it->timer) { it->timer->stop(); it->timer->deleteLater(); }
            if (it->callback)
                it->callback({{"notice", tr("Desktop worker restarted. Run 'sol history' to copy.")}});
        }
    });
    desktopWorker.start(python(), {"-u", scripts.filePath("desktop.py"), "--worker"});
}

void Desktop::handleWorkerResponse(const QJsonObject &msg)
{
    const int id = msg.value("id").toInt();
    if (!workerJobs.contains(id)) return;
    auto entry = workerJobs.take(id);
    if (entry.timer) {
        entry.timer->stop();
        entry.timer->deleteLater();
    }
    QJsonObject result = msg;
    result.remove("id");
    if (result.isEmpty())
        result["notice"] = tr("Desktop integration timed out. Run 'sol history' to copy.");
    if (result.value("unsupported").toBool() && portalReady && input.state() == QProcess::Running) {
        QJsonObject paste = entry.job;
        paste["op"] = "paste";
        if (entry.job.value("op").toString() == "paste_text") paste["relaxed"] = true;
        const int pid = ++nextRequest;
        paste["id"] = pid;
        deliveries.insert(pid, entry.callback);
        input.write(QJsonDocument(paste).toJson(QJsonDocument::Compact) + '\n');
        QTimer::singleShot(6000, this, [this, pid] {
            auto done = deliveries.take(pid);
            if (done) done({{"notice", tr("Desktop paste timed out. Run 'sol history' to copy.")}});
        });
    } else if (entry.callback) {
        entry.callback(result);
    }
}

void Desktop::configure()
{
    if (nativeKeys) {
        info = tr("Hyprland shortcut active. Run 'sol shortcuts setup' to change.");
        emit shortcutsChanged();
    } else if (shortcuts.state() == QProcess::Running) {
        shortcuts.write("configure\n");
    } else {
        info = tr("Run 'sol shortcuts' to view or apply bindings for your desktop.");
        emit shortcutsChanged();
    }
}

void Desktop::rebindX11(const QString &holdChord)
{
    if (nativeKeys) {
        hold = holdChord;
        emit shortcutsChanged();
        return;
    }
    if (!x11Session || shortcuts.state() != QProcess::Running) return;
    const QJsonObject payload{{"op", "rebind"}, {"hold", holdChord}};
    shortcuts.write(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
}

void Desktop::configureInput()
{
    if (input.state() == QProcess::Running)
        input.write(portalReady ? "{\"op\":\"close\"}\n" : "{\"op\":\"configure\"}\n");
    else
        emit shortcutsChanged();
}

void Desktop::request(const QJsonObject &job, std::function<void(QJsonObject)> callback)
{
    ensureDesktopWorker();
    if (desktopWorker.state() != QProcess::NotRunning) {
        const int id = ++nextRequest;
        QJsonObject tagged = job;
        tagged["id"] = id;
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, id] {
            if (!workerJobs.contains(id)) return;
            auto entry = workerJobs.take(id);
            if (entry.timer) entry.timer->deleteLater();
            if (entry.callback)
                entry.callback({{"notice", tr("Desktop integration timed out.")}});
        });
        timer->start(3500);
        workerJobs.insert(id, {job, std::move(callback), timer});
        if (desktopWorkerReady)
            desktopWorker.write(QJsonDocument(tagged).toJson(QJsonDocument::Compact) + '\n');
        else
            workerQueue.append({id, tagged});
        return;
    }

    auto *p = new QProcess(this);
    auto finish = [this, p, callback, job] {
        if (p->property("answered").toBool()) return;
        p->setProperty("answered", true);
        auto result = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        if (result.isEmpty())
            result["notice"] = tr("Desktop integration unavailable.");
        if (result["unsupported"].toBool() && portalReady && input.state() == QProcess::Running) {
            auto paste = job;
            paste["op"] = "paste";
            const int id = ++nextRequest;
            paste["id"] = id;
            deliveries.insert(id, callback);
            input.write(QJsonDocument(paste).toJson(QJsonDocument::Compact) + '\n');
            QTimer::singleShot(6000, this, [this, id] {
                auto done = deliveries.take(id);
                if (done) done({{"notice", tr("Desktop paste timed out.")}});
            });
        } else if (callback) {
            callback(result);
        }
        p->deleteLater();
    };
    connect(p, &QProcess::finished, this, finish);
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish();
    });
    p->start(python(), {"-c", code("desktop")});
    p->write(QJsonDocument(job).toJson(QJsonDocument::Compact));
    p->closeWriteChannel();
    QTimer::singleShot(3500, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
}

void Desktop::notify(const QString &message, bool transient)
{
    if (message.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        QDBusInterface notif("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                             "org.freedesktop.Notifications", bus);
        notif.setTimeout(300);
        QVariantMap hints{{"transient", true}, {"desktop-entry", QStringLiteral("org.sol.Dictation")}};
        QDBusReply<quint32> reply = notif.call(
            "Notify", QStringLiteral("Sol"), transient ? notifyId : 0u, QStringLiteral("sol"),
            QStringLiteral("Sol"), message, QStringList(), hints, transient ? 1800 : 2800);
        if (reply.isValid()) {
            if (transient) notifyId = reply.value();
            return;
        }
    }
    const QString notifySend = QStandardPaths::findExecutable("notify-send");
    if (!notifySend.isEmpty())
        QProcess::startDetached(notifySend, {"--app-name=Sol", "--expire-time=2800", "Sol", message});
}

void Desktop::ensureHud(const QString &theme)
{
    const QString cleanTheme = (theme == "mono") ? QStringLiteral("mono") : QStringLiteral("glass");
    if (!hudAvailable || hudFailed) return;
    if (hud.state() != QProcess::NotRunning) {
        if (activeHudTheme == cleanTheme) return;
        // Theme changed (mono <-> glass): restart _hud so LayerShell namespace updates cleanly.
        hud.disconnect(this);
        hud.closeWriteChannel();
        if (!hud.waitForFinished(300)) {
            hud.kill();
            hud.waitForFinished(500);
        }
    }
    activeHudTheme = cleanTheme;
    hud.disconnect(this);
    connect(&hud, &QProcess::readyReadStandardOutput, this, [this] {
        while (hud.canReadLine()) {
            const auto msg = QJsonDocument::fromJson(hud.readLine()).object();
            if (!msg.isEmpty()) emit hudMessage(msg);
        }
    });
    connect(&hud, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (exitCode != 0 || status != QProcess::NormalExit)
            hudFailed = true;
    });
    auto env = QProcessEnvironment::systemEnvironment();
    if (hudUseXcb) env.insert("QT_QPA_PLATFORM", "xcb");
    hud.setProcessEnvironment(env);
    hud.start(QCoreApplication::applicationFilePath(), {"_hud", cleanTheme});
}

void Desktop::indicate(const QJsonObject &hudState)
{
    const QString phase = hudState.value("phase").toString("idle");
    const QString theme = hudState.value("theme").toString("glass");
    const bool enabled = hudState.value("hud").toBool(true);
    const bool hasToast = !hudState.value("toast").toString().isEmpty();

    if (!enabled) {
        if (hud.state() != QProcess::NotRunning) {
            QJsonObject hide = hudState;
            hide["phase"] = "idle";
            hide["hud"] = false;
            hide["toast"] = "";
            hud.write(QJsonDocument(hide).toJson(QJsonDocument::Compact) + '\n');
        }
        lastPhase = phase;
        return;
    }

    const bool shouldShow = phase != "idle" || hasToast;
    if (hudAvailable && !hudFailed && (shouldShow || (hud.state() != QProcess::NotRunning && activeHudTheme != theme)))
        ensureHud(theme);

    if (hud.state() != QProcess::NotRunning) {
        hud.write(QJsonDocument(hudState).toJson(QJsonDocument::Compact) + '\n');
    } else if (phase != lastPhase) {
        if (phase == "recording") notify(tr("Listening..."), true);
        else if (phase == "processing") notify(tr("Transcribing..."), true);
    }
    lastPhase = phase;
}
