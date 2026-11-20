#include "cli_ui.h"

#include <QDateTime>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <poll.h>
#include <unistd.h>

namespace CliUi {

namespace {
QString wrap(const char *code, const QString &text)
{
    if (!useColor()) return text;
    return QString::fromLatin1(code) + text + QStringLiteral("\033[0m");
}

QString padPlain(const QString &text, int width)
{
    return text.size() >= width ? text : text + QString(width - text.size(), ' ');
}

QString readPromptLine(QTextStream &out, const QString &promptText)
{
    out << promptText << Qt::flush;
    QTextStream in(stdin);
    if (in.atEnd()) return {};
    return in.readLine().trimmed();
}
} // namespace

bool useColor()
{
    if (!qEnvironmentVariableIsEmpty("NO_COLOR")) return false;
    if (!qEnvironmentVariableIsEmpty("FORCE_COLOR")) return true;
    return isatty(STDOUT_FILENO) == 1;
}

QString good(const QString &text) { return wrap("\033[1;32m", text); }
QString bad(const QString &text) { return wrap("\033[1;31m", text); }
QString warn(const QString &text) { return wrap("\033[1;33m", text); }
QString imp(const QString &text) { return wrap("\033[1;38;5;221m", text); } // solar gold
QString key(const QString &text) { return wrap("\033[1;36m", text); }        // cyan for shortcut keys
QString cmd(const QString &text) { return wrap("\033[1;38;5;221m", text); }  // gold for commands
QString strong(const QString &text) { return wrap("\033[1;97m", text); }
QString dim(const QString &text) { return wrap("\033[2m", text); }

QString badgeOnOff(bool value)
{
    return value ? good("[ON] ") : dim("[OFF]");
}

QString badgeGood(const QString &label)
{
    return useColor() ? QStringLiteral("\033[1;32m[%1]\033[0m").arg(label) : QStringLiteral("[%1]").arg(label);
}

QString badgeBad(const QString &label)
{
    return useColor() ? QStringLiteral("\033[1;31m[%1]\033[0m").arg(label) : QStringLiteral("[%1]").arg(label);
}

QString badgeWarn(const QString &label)
{
    return useColor() ? QStringLiteral("\033[1;33m[%1]\033[0m").arg(label) : QStringLiteral("[%1]").arg(label);
}

QString badgeImp(const QString &label)
{
    return useColor() ? QStringLiteral("\033[1;38;5;221m[%1]\033[0m").arg(label) : QStringLiteral("[%1]").arg(label);
}

void printBanner(QTextStream &out, const QString &view)
{
    out << "\n  " << imp("sol");
    if (!view.isEmpty()) out << " " << dim("/") << " " << strong(view);
    out << "\n";
}

void printSection(QTextStream &out, const QString &title)
{
    QString visible = title;
    visible.remove(QRegularExpression(R"(\x1b\[[0-9;]*m)"));
    const int ruleLen = std::max(8, 60 - static_cast<int>(visible.size()));
    out << "\n  " << strong(title) << " " << dim(QString(ruleLen, '-')) << "\n\n";
}

void printRow(QTextStream &out, const QString &marker, const QString &label,
              const QString &value, const QString &hint)
{
    out << "  " << marker << " " << strong(padPlain(label, 24)) << " " << value;
    if (!hint.isEmpty()) out << "   " << dim(hint);
    out << "\n";
}

void printHelp(QTextStream &out)
{
    printBanner(out);

    printSection(out, "Core");
    out << "  " << cmd(padPlain("sol", 36)) << "Start background daemon and show status\n"
        << "  " << cmd(padPlain("sol setup [cuda|cpu] [model]", 36)) << "Install model, pick languages and shortcut\n"
        << "  " << cmd(padPlain("sol settings [KEY [VALUE]]", 36)) << "Interactive settings (or set one option)\n"
        << "  " << cmd(padPlain("sol shortcuts [show|setup|apply]", 36)) << "View, change, or apply dictation shortcut\n"
        << "  " << cmd(padPlain("sol doctor [--json]", 36)) << "Check GPU, audio, clipboard, HUD, shortcut\n"
        << "  " << cmd(padPlain("sol dictate", 36)) << "Quick voice test in terminal\n"
        << "  " << cmd(padPlain("sol quit", 36)) << "Stop background daemon\n"
        << "  " << cmd(padPlain("sol uninstall [--purge] [-y]", 36)) << "Uninstall Sol\n";

    printSection(out, "Dictation Controls");
    out << "  " << cmd(padPlain("sol start / sol stop", 36)) << "Hold to speak, release to paste\n"
        << "  " << cmd(padPlain("sol toggle", 36)) << "Start or stop recording on single press\n"
        << "  " << cmd(padPlain("sol cancel", 36)) << "Cancel active dictation\n"
        << "  " << cmd(padPlain("sol paste-last", 36)) << "Paste last dictation again\n"
        << "  " << cmd(padPlain("sol transcribe AUDIO", 36)) << "Transcribe an audio file offline\n";

    printSection(out, "Vocabulary, Style & History");
    out << "  " << cmd(padPlain("sol style [show|auto|<tone>]", 36)) << "Per-app writing tones\n"
        << "  " << cmd(padPlain("sol dict [list|add|remove]", 36)) << "Personal dictionary\n"
        << "  " << cmd(padPlain("sol snippet [list|add|remove]", 36)) << "Voice snippets ({{date}}, {{clipboard}})\n"
        << "  " << cmd(padPlain("sol mic [list|set N|test]", 36)) << "Select microphone or run live level test\n"
        << "  " << cmd(padPlain("sol history [list|copy|raw|clear]", 36)) << "Browse, copy, edit, or clear history\n"
        << "  " << cmd(padPlain("sol stats [--json]", 36)) << "Streak, words, WPM, time saved\n"
        << "  " << cmd(padPlain("sol logs [-f]", 36)) << "View daemon log\n\n";
}

void printStatusCard(QTextStream &out, const QJsonObject &s, bool daemonRunning)
{
    printBanner(out, "status");
    out << "\n";

    if (daemonRunning)
        printRow(out, good("+"), "Daemon", badgeGood("RUNNING") + " " + dim("state: " + s.value("state").toString("idle")));
    else
        printRow(out, bad("x"), "Daemon", badgeBad("STOPPED"), "run: sol");

    out << "\n";

    if (s.value("speechReady").toBool()) {
        const QString m = s.value("speechModel").toString("whisper");
        const QString d = s.value("speechDevice").toString("local").toUpper();
        printRow(out, good("+"), "Model", badgeGood("READY") + " " + imp(m) + " " + dim("(" + d + ")"));
    } else {
        printRow(out, bad("x"), "Model", badgeBad("MISSING"), "fix: " + cmd("sol setup"));
    }

    const QString langs = s.value("languagesDisplay").toString(s.value("language").toString("auto"));
    printRow(out, imp(">"), "Languages", imp(langs), "change: " + cmd("sol settings"));

    out << "\n";

    const bool active = s.value("shortcutActive").toBool();
    const QString shortcutKey = s.value("shortcut").toString();
    if (active) {
        printRow(out, good("+"), "Shortcut", badgeGood("ACTIVE") + " " + key(shortcutKey), "hold to speak, release to paste");
    } else {
        printRow(out, warn("!"), "Shortcut", badgeWarn("NOT BOUND"), "fix: " + cmd("sol shortcuts setup"));
    }

    const bool hud = s.value("showHud").toBool(true);
    const QString theme = s.value("hudTheme").toString("glass");
    const bool whisper = s.value("whisperMode").toBool();
    printRow(out, imp(">"), "Floating HUD",
             hud ? good(theme == "mono" ? "black & white (mono)" : "frosted glass (glass)") : dim("off"),
             "whisper: " + badgeOnOff(whisper));

    out << "\n";
    if (!s.value("speechReady").toBool()) {
        out << "  " << bad("Next:") << " run " << cmd("sol setup") << "\n\n";
    } else if (!active) {
        out << "  " << warn("Next:") << " run " << cmd("sol shortcuts setup") << " (or test with " << cmd("sol dictate") << ")\n\n";
    } else {
        out << "  " << good("Ready:") << " hold " << key(shortcutKey) << " in any app to speak and paste.\n"
            << "  " << dim("Commands:") << " " << cmd("sol settings")
            << dim("  |  ") << cmd("sol doctor")
            << dim("  |  ") << cmd("sol history") << "\n\n";
    }
}

void printDoctor(QTextStream &out, const QJsonObject &probe)
{
    printBanner(out, "doctor");
    const auto progs = probe.value("programs").toObject();
    const QString session = probe.value("session").toString();
    const QString desktop = probe.value("desktop").toString();
    const bool isWayland = session == "wayland" || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    const bool isX11 = !isWayland && !qEnvironmentVariableIsEmpty("DISPLAY");

    struct Item { QString label; QString detail; };
    QList<Item> goodItems, warnItems, badItems;
    auto addGood = [&](const QString &l, const QString &d) { goodItems.append(Item{l, d}); };
    auto addWarn = [&](const QString &l, const QString &d) { warnItems.append(Item{l, d}); };
    auto addBad = [&](const QString &l, const QString &d) { badItems.append(Item{l, d}); };

    if (probe.value("speechReady").toBool())
        addGood("Speech model", probe.value("speechModel").toString() + " on " + probe.value("speechDevice").toString().toUpper());
    else
        addBad("Speech model missing", "Run: " + cmd("sol setup"));

    if (!progs["nvidia-smi"].toString().isEmpty())
        addGood("GPU", "NVIDIA CUDA available");
    else
        addWarn("GPU", "No NVIDIA GPU detected (using CPU int8)");

    QStringList recTools;
    for (auto *t : {"parec", "pw-record", "arecord", "ffmpeg"})
        if (!progs[t].toString().isEmpty()) recTools << t;
    if (!recTools.isEmpty()) addGood("Mic capture", recTools.join(", "));
    else addBad("Mic capture missing", "Install pipewire-utils, pulseaudio-utils, or alsa-utils");

    QStringList playTools;
    for (auto *t : {"paplay", "pw-play", "aplay"})
        if (!progs[t].toString().isEmpty()) playTools << t;
    if (!playTools.isEmpty()) addGood("Audio chimes", playTools.join(", "));
    else addWarn("Audio chimes", "Install pipewire-utils, pulseaudio-utils, or alsa-utils");

    addGood("Session", (desktop.isEmpty() ? "Unknown" : desktop) + " (" + (isWayland ? "Wayland" : isX11 ? "X11" : "Headless") + ")");

    if (isWayland) {
        if (!progs["wl-copy"].toString().isEmpty() && !progs["wl-paste"].toString().isEmpty())
            addGood("Clipboard", "wl-copy, wl-paste");
        else
            addBad("wl-clipboard missing", "Install wl-clipboard for Wayland pasting");

        QStringList p;
        for (auto *t : {"hyprctl", "wtype", "ydotool", "dotool"})
            if (!progs[t].toString().isEmpty()) p << t;
        if (!p.isEmpty()) addGood("Paste driver", p.join(", "));
        else addWarn("Paste driver", "Using AT-SPI2 / XDG portal (or install wtype / ydotool)");

        if (probe.value("layerShellCompiled").toBool())
            addGood("Floating HUD", "LayerShellQt compiled");
        else
            addWarn("Floating HUD", "Built without LayerShellQt (uses fallback)");
    } else if (isX11) {
        if (!progs["xdotool"].toString().isEmpty()) addGood("Paste driver", "xdotool");
        else addBad("xdotool missing", "Install xdotool for X11 window detection and pasting");
        if (!progs["xclip"].toString().isEmpty() || !progs["xsel"].toString().isEmpty())
            addGood("Clipboard", !progs["xclip"].toString().isEmpty() ? "xclip" : "xsel");
        else
            addWarn("Clipboard tool missing", "Install xclip or xsel");
    }

    if (probe.value("shortcutActive").toBool())
        addGood("Shortcut", "Active (" + probe.value("shortcut").toString() + ")");
    else
        addWarn("Shortcut not bound", "Run: " + cmd("sol shortcuts setup"));

    addGood("Languages", probe.value("languagesDisplay").toString(probe.value("language").toString("auto")));

    if (!badItems.isEmpty()) {
        printSection(out, bad("Needs Attention"));
        for (const auto &it : badItems)
            printRow(out, bad("x"), it.label, bad(it.detail));
    }

    if (!warnItems.isEmpty()) {
        printSection(out, warn("Warnings / Optional"));
        for (const auto &it : warnItems)
            printRow(out, warn("!"), it.label, it.detail);
    }

    printSection(out, good("Working"));
    for (const auto &it : goodItems)
        printRow(out, good("+"), it.label, good(it.detail));

    out << "\n";
}

void printSettingsSnapshot(QTextStream &out, const Backend &b)
{
    printBanner(out, "settings");

    printSection(out, "Shortcut & HUD");
    out << "  " << imp("[1] ") << strong(padPlain("Floating HUD", 24))
        << (b.showHud() ? (badgeOnOff(true) + " " + imp(b.hudTheme())) : badgeOnOff(false))
        << "  " << dim("(glass | mono | off)") << "\n\n";
    out << "  " << imp("[2] ") << strong(padPlain("Double-tap lock", 24)) << badgeOnOff(b.doubleTapLock())
        << "  " << dim("tap twice to keep speaking without holding") << "\n\n";
    out << "  " << imp("[s] ") << strong(padPlain("Dictation shortcut", 24)) << key(b.shortcut())
        << "  " << dim("hold to speak, release to paste") << "\n";

    printSection(out, "Voice & Languages");
    out << "  " << imp("[3] ") << strong(padPlain("Languages", 24)) << imp(b.languagesDisplay())
        << "  " << dim("single, multi-language, or auto") << "\n\n";
    out << "  " << imp("[4] ") << strong(padPlain("Whisper Mode", 24)) << badgeOnOff(b.whisperMode())
        << "  " << dim("boost quiet speech") << "\n\n";
    out << "  " << imp("[5] ") << strong(padPlain("Sound chimes", 24)) << badgeOnOff(b.soundEffects())
        << "  " << dim("start / stop audio cues") << "\n\n";
    out << "  " << imp("[6] ") << strong(padPlain("Pause media", 24)) << badgeOnOff(b.pauseMedia())
        << "  " << dim("pause music while speaking") << "\n\n";
    out << "  " << imp("[7] ") << strong(padPlain("Microphone", 24)) << imp(b.microphone()) << "\n";

    printSection(out, "Formatting");
    out << "  " << imp("[8] ") << strong(padPlain("Cleanup", 24)) << imp(b.cleanup())
        << "  " << dim("(none | light | medium)") << "\n\n";
    out << "  " << imp("[9] ") << strong(padPlain("Writing style", 24)) << imp(b.style())
        << "  " << dim("(auto | formal | casual | lowercase | excited | verbatim)") << "\n\n";
    out << "  " << imp("[10]") << " " << strong(padPlain("Code Mode", 23)) << imp(b.codeMode())
        << "  " << dim("(auto | on | off)") << "\n\n";
    out << "  " << imp("[11]") << " " << strong(padPlain("Voice @file tagging", 23)) << badgeOnOff(b.fileTagging())
        << "  " << dim("'at main dot cpp' -> @main.cpp") << "\n";

    printSection(out, "Pasting & Privacy");
    out << "  " << imp("[12]") << " " << strong(padPlain("Auto-paste at cursor", 23)) << badgeOnOff(b.autoInsert()) << "\n\n";
    out << "  " << imp("[13]") << " " << strong(padPlain("Preserve clipboard", 23)) << badgeOnOff(b.restoreClipboard())
        << "  " << dim("restores prior clipboard after paste") << "\n\n";
    out << "  " << imp("[14]") << " " << strong(padPlain("App context", 23)) << badgeOnOff(b.contextAware())
        << "  " << dim("match app tone, casing & symbols") << "\n\n";
    out << "  " << imp("[15]") << " " << strong(padPlain("Save history", 23)) << badgeOnOff(b.keepHistory()) << "\n\n";
    out << "  " << imp("[16]") << " " << strong(padPlain("Start at login", 23)) << badgeOnOff(b.autostart()) << "\n";
}

void printStyleOverview(QTextStream &out, const Backend &b)
{
    printBanner(out, "style");
    printSection(out, "Master Mode");
    printRow(out, imp(">"), "Mode", imp(b.style()), b.style() == "auto" ? "adapts per focused app" : "fixed style everywhere");

    printSection(out, "Per-App Categories");
    printRow(out, imp("-"), "Personal chat", imp(b.personalStyle()), "Signal, Telegram, WhatsApp, Discord");
    printRow(out, imp("-"), "Work chat", imp(b.workStyle()), "Slack, Teams, Mattermost, Linear, Notion");
    printRow(out, imp("-"), "Email", imp(b.emailStyle()), "Gmail, Thunderbird, Outlook, Proton Mail");
    printRow(out, imp("-"), "Code & terminals", imp(b.codeStyle()), "VS Code, Cursor, Zed, Neovim, Kitty, Ghostty");
    printRow(out, imp("-"), "Other apps", imp(b.otherStyle()), "Browsers, docs, everything else");

    const auto rules = b.appRules();
    if (!rules.isEmpty()) {
        printSection(out, "Custom App Rules");
        for (const auto &v : rules) {
            const auto m = v.toMap();
            printRow(out, good("+"), m["app"].toString(), imp(m["style"].toString()));
        }
    }

    printSection(out, "Preview");
    const QString sample = "um hey, let's meet Monday actually Tuesday at 3 30 pm to review the launch";
    out << "  " << dim("Input:    ") << dim(sample) << "\n\n";
    for (const char *t : {"formal", "casual", "lowercase", "excited", "verbatim"}) {
        const QString tone = QString::fromLatin1(t);
        out << "  " << imp(padPlain(tone, 11)) << strong(b.previewStyle(sample, tone)) << "\n";
    }
    out << "\n  " << dim("Set mode:  ") << cmd("sol style auto|formal|casual|lowercase|excited|verbatim")
        << "\n  " << dim("Category:  ") << cmd("sol style cat personal lowercase")
        << "\n  " << dim("App rule:  ") << cmd("sol style app obsidian formal") << "\n\n";
}

void printStats(QTextStream &out, const Backend &b)
{
    printBanner(out, "stats");
    printSection(out, "Summary");
    printRow(out, good("+"), "Daily streak", imp(QString::number(b.streakDays()) + " days"));
    printRow(out, good("+"), "Words dictated", imp(QString::number(b.totalWords())) + dim(" (" + QString::number(b.totalDictations()) + " dictations)"));
    printRow(out, good("+"), "Average speed", b.averageWpm() > 0 ? imp(QString::number(b.averageWpm()) + " WPM") : dim("not enough timed speech yet"));
    printRow(out, good("+"), "Est. time saved", imp(QString::number(b.minutesSaved()) + " min") + dim(" (vs 45 WPM typing)"));
    out << "\n";
}

void printEntries(QTextStream &out, const QString &kind, const QVariantList &rows)
{
    const bool isDict = (kind == "dictionary");
    printBanner(out, isDict ? "dictionary" : "snippets");
    out << "\n";
    if (rows.isEmpty()) {
        out << "  " << warn("!") << " " << (isDict ? "Dictionary is empty." : "No snippets saved yet.") << "\n\n"
            << "  " << dim("Add via CLI:  ")
            << cmd(isDict ? "sol dict add kube \"Kubernetes\"" : "sol snippet add \"my email\" \"you@example.com\"")
            << "\n";
        if (isDict)
            out << "  " << dim("Or click pill: ") << imp("arm -> ARM") << dim("  (or ") << imp("slash = /") << dim(")") << "\n";
        out << "\n";
        return;
    }
    for (const auto &v : rows) {
        const auto m = v.toMap();
        out << "  " << imp(padPlain(m["trigger"].toString(), 22)) << " " << dim("->") << " " << strong(m["value"].toString());
        if (m["autoLearned"].toBool()) out << "  " << badgeGood("auto-learned");
        out << "\n";
    }
    out << "\n";
}

void printHistory(QTextStream &out, const QVariantList &rows, int limit, const QString &search)
{
    printBanner(out, "history");
    out << "\n";
    QVariantList filtered;
    for (const auto &v : rows) {
        const auto m = v.toMap();
        if (!search.isEmpty()
            && !m["text"].toString().contains(search, Qt::CaseInsensitive)
            && !m["app"].toString().contains(search, Qt::CaseInsensitive))
            continue;
        filtered.append(v);
        if (filtered.size() >= limit) break;
    }
    if (filtered.isEmpty()) {
        out << "  " << warn("!") << " " << (search.isEmpty() ? "No dictations recorded yet." : "No dictations match that search.") << "\n\n";
        return;
    }
    for (int i = 0; i < filtered.size(); ++i) {
        const auto m = filtered[i].toMap();
        const QString ts = m["created"].toString().left(16).replace('T', ' ');
        const QString app = m["app"].toString();
        const QString text = m["text"].toString();
        const QString raw = m["raw"].toString();
        out << "  " << imp(QString("[%1]").arg(i + 1)) << " " << dim(ts);
        if (!app.isEmpty()) out << "  " << badgeImp(app);
        out << "\n      " << strong(text) << "\n";
        if (!raw.isEmpty() && raw.trimmed() != text.trimmed())
            out << "      " << dim("raw: " + raw) << "\n";
        out << "\n";
    }
    out << "  " << dim("Actions:") << " "
        << cmd("sol history copy 1") << dim(" | ")
        << cmd("sol history raw 1") << dim(" | ")
        << cmd("sol history edit 1 \"corrected text\"") << dim(" | ")
        << cmd("sol history clear") << "\n\n";
}

bool interactiveLanguageSetup(Backend &b, QTextStream &out,
                              const std::function<void()> &reloadDaemon)
{
    printSection(out, "Languages");
    out << "  Current: " << imp(b.languagesDisplay()) << "\n\n"
        << "  Which language(s) will you speak?\n\n"
        << "    " << imp("[1]") << " " << strong(padPlain("English only (en)", 30)) << good("Fastest & highest accuracy") << "\n"
        << "    " << imp("[2]") << " " << strong(padPlain("Multiple languages", 30)) << dim("e.g. en, hu or en, de (auto-switches cleanly)") << "\n"
        << "    " << imp("[3]") << " " << strong(padPlain("Single other language", 30)) << dim("ISO code: hu, de, es, fr, ja, ...") << "\n"
        << "    " << imp("[4]") << " " << strong(padPlain("Auto-detect all languages", 30)) << dim("any language (may guess wrong on 1-word clips)") << "\n"
        << "    " << dim("[0] Keep current (" + b.languagesDisplay() + ")") << "\n\n";

    const QString choice = readPromptLine(out, "  " + strong("Language choice") + " " + dim("[1]") + ": ");
    if (choice == "0") {
        out << "\n  " << dim("Kept current language setting: " + b.languagesDisplay()) << "\n";
        return false;
    }

    if (choice.isEmpty() || choice == "1") {
        b.setLanguages("en", {});
    } else if (choice == "2") {
        const QString raw = readPromptLine(out, "  " + strong("Enter language codes") + " " + dim("(comma-separated, e.g. en, hu)") + ": ");
        const QStringList parts = raw.toLower().split(QRegularExpression(R"([,\s]+)"), Qt::SkipEmptyParts);
        if (parts.isEmpty() || !b.setLanguages(parts.size() == 1 ? parts.first() : QStringLiteral("auto"), parts)) {
            out << "  " << bad("x") << " Invalid language codes. Use 2-letter codes like en, hu, de.\n";
            return false;
        }
    } else if (choice == "3") {
        const QString code = readPromptLine(out, "  " + strong("Enter language code") + " " + dim("(e.g. hu, de, es, fr, ja)") + ": ").toLower();
        if (!b.setLanguages(code, {})) {
            out << "  " << bad("x") << " Invalid language code. Use a 2-letter ISO code like hu, de, es.\n";
            return false;
        }
    } else if (choice == "4") {
        b.setLanguages("auto", {});
    } else {
        // Allow typing codes directly, e.g. "en, hu" or "hu".
        const QStringList parts = choice.toLower().split(QRegularExpression(R"([,\s]+)"), Qt::SkipEmptyParts);
        if (parts.isEmpty() || !b.setLanguages(parts.size() == 1 ? parts.first() : QStringLiteral("auto"), parts)) {
            out << "  " << bad("x") << " Unrecognized language input: " << choice << "\n";
            return false;
        }
    }

    if (reloadDaemon) reloadDaemon();
    out << "\n  " << good("+") << " Languages set to: " << imp(b.languagesDisplay()) << "\n";
    return true;
}

bool interactiveHudThemeSetup(Backend &b, QTextStream &out,
                              const std::function<void()> &reloadDaemon)
{
    const QString curLabel = !b.showHud() ? QStringLiteral("off")
        : (b.hudTheme() == "mono" ? QStringLiteral("mono (black & white)") : QStringLiteral("glass (frosted blur)"));
    printSection(out, "HUD Design");
    out << "  Current: " << imp(curLabel) << "\n\n"
        << "  Choose the floating pill design shown while speaking:\n\n"
        << "    " << imp("[1]") << " " << strong(padPlain("Frosted Glass (glass)", 26))
        << good("Blurred translucent glass matching what's behind it") << "\n"
        << "    " << imp("[2]") << " " << strong(padPlain("Black & White (mono)", 26))
        << dim("Pure black background, white waveform, no border") << "\n"
        << "    " << imp("[3]") << " " << strong(padPlain("Off", 26))
        << dim("No floating pill (audio chimes only)") << "\n"
        << "    " << dim("[0] Keep current (" + curLabel + ")") << "\n\n";

    const QString choice = readPromptLine(out, "  " + strong("HUD design") + " " + dim("[1]") + ": ").toLower();
    if (choice == "0") {
        out << "\n  " << dim("Kept current HUD design: " + curLabel) << "\n";
        return false;
    }

    if (choice.isEmpty() || choice == "1" || choice == "glass" || choice == "blur") {
        b.setOption("showHud", true);
        b.setOption("hudTheme", "glass");
    } else if (choice == "2" || choice == "mono" || choice == "bw" || choice == "black") {
        b.setOption("showHud", true);
        b.setOption("hudTheme", "mono");
    } else if (choice == "3" || choice == "off" || choice == "none") {
        b.setOption("showHud", false);
    } else {
        out << "  " << warn("!") << " Unrecognized choice: " << choice << "\n";
        return false;
    }

    if (Desktop::hasActiveHyprlandBinds()) {
        const bool glassBlur = b.showHud() && b.hudTheme() == "glass";
        Desktop::applyLiveShortcuts(b.shortcut(), glassBlur);
    }
    if (reloadDaemon) reloadDaemon();
    const QString nextLabel = !b.showHud() ? QStringLiteral("off")
        : (b.hudTheme() == "mono" ? QStringLiteral("black & white (mono)") : QStringLiteral("frosted glass (glass)"));
    out << "\n  " << good("+") << " HUD design set to: " << imp(nextLabel) << "\n";
    return true;
}

bool interactiveShortcutSetup(Backend &b, Desktop &d, QTextStream &out,
                              const std::function<void()> &reloadDaemon,
                              const std::function<void()> &portalBind)
{
    printSection(out, "Dictation Shortcut");
    const QString de = d.desktopEnvironment();
    out << "  " << imp(">") << " Desktop: " << strong(de)
        << " " << dim(d.isWayland() ? "(Wayland)" : d.isX11() ? "(X11)" : "") << "\n"
        << "  " << dim("Hold the key to speak, release to paste. Double-tap to keep speaking without holding.") << "\n\n"
        << "    " << imp("[1]") << " " << key(padPlain("F9", 18)) << good("Recommended") << dim(" -- single-key hold") << "\n"
        << "    " << imp("[2]") << " " << key(padPlain("Super+Alt+V", 18)) << dim("Safe across all apps") << "\n"
        << "    " << imp("[3]") << " " << key(padPlain("Ctrl+Alt+Space", 18)) << dim("Classic dictation chord") << "\n"
        << "    " << imp("[4]") << " " << key(padPlain("Alt_R", 18)) << dim("Right Alt single-key hold") << "\n"
        << "    " << imp("[5]") << " " << strong(padPlain("Custom...", 18)) << dim("Enter any key or combo (e.g. F8, Super+D)") << "\n"
        << "    " << dim("[0] Keep current (" + b.shortcut() + ")") << "\n\n";

    const QString choice = readPromptLine(out, "  " + strong("Shortcut choice") + " " + dim("[1]") + ": ");
    QString hold = b.shortcut();
    if (choice == "0") {
        out << "\n  " << dim("Kept current shortcut: " + hold) << "\n";
        return false;
    } else if (choice.isEmpty() || choice == "1") {
        hold = "F9";
    } else if (choice == "2") {
        hold = "Super+Alt+V";
    } else if (choice == "3") {
        hold = "Ctrl+Alt+Space";
    } else if (choice == "4") {
        hold = "Alt_R";
    } else if (choice == "5") {
        const QString custom = readPromptLine(out, "  " + strong("Enter shortcut") + " " + dim("(e.g. F8, Super+Alt+D, Ctrl+Space)") + ": ");
        if (!custom.isEmpty()) hold = custom;
    } else {
        hold = choice;
    }

    b.setOption("shortcut", hold);
    if (reloadDaemon) reloadDaemon();

    out << "\n  " << good("+") << " Saved shortcut: " << key(hold) << "\n";

    const bool glassBlur = b.showHud() && b.hudTheme() == "glass";
    QString liveMsg;
    if (Desktop::applyLiveShortcuts(hold, glassBlur, &liveMsg)) {
        out << "  " << good("+") << " " << good(liveMsg) << "\n";
        const QString persistAns = readPromptLine(out,
            "\n  " + strong("Save shortcut across " + de + " reloads?") + " " + dim("[Y/n]") + ": ");
        if (persistAns.isEmpty() || persistAns.toLower().startsWith('y')) {
            QString persistMsg;
            if (Desktop::persistShortcuts(hold, glassBlur, &persistMsg))
                out << "  " << good("+") << " " << persistMsg << "\n";
            else
                out << "  " << warn("!") << " " << persistMsg << "\n";
        }
    } else if (d.isX11()) {
        out << "  " << good("+") << " Active immediately on X11 via background daemon.\n";
    } else {
        if (portalBind && (de.toLower().contains("kde") || de.toLower().contains("plasma")
                           || de.toLower().contains("gnome") || de.toLower().contains("cosmic"))) {
            const QString portalAns = readPromptLine(out,
                "\n  " + strong("Open system Global Shortcuts dialog now?") + " " + dim("[Y/n]") + ": ");
            if (portalAns.isEmpty() || portalAns.toLower().startsWith('y')) {
                portalBind();
                out << "  " << good("+") << " Sent shortcut bind request to your desktop portal.\n";
            }
        }
        out << "\n  " << imp("Manual config for " + de + ":") << "\n\n";
        for (const QString &line : d.shortcutSnippet(hold).split('\n'))
            out << "    " << (line.startsWith('#') || line.startsWith("//") ? dim(line) : key(line)) << "\n";
    }
    out << "\n";
    return true;
}

void interactiveSettings(Backend &b, Desktop &d, QTextStream &out,
                         const std::function<void()> &reloadDaemon,
                         const std::function<void()> &portalBind)
{
    auto cycle = [&](const QString &keyName, const QStringList &values, const QString &current) {
        const int idx = values.indexOf(current);
        const QString next = values.value((idx + 1) % values.size());
        b.setOption(keyName, next);
        if (reloadDaemon) reloadDaemon();
        out << "\n  " << good("+") << " " << strong(keyName) << " -> " << imp(next) << "\n";
    };
    auto flip = [&](const QString &keyName, bool current) {
        b.setOption(keyName, !current);
        if (reloadDaemon) reloadDaemon();
        out << "\n  " << good("+") << " " << strong(keyName) << " -> " << badgeOnOff(!current) << "\n";
    };

    while (true) {
        printSettingsSnapshot(out, b);
        out << "\n  " << dim("Enter:") << " "
            << imp("[1-16]") << dim(" change  |  ")
            << imp("[s]") << dim(" shortcut  |  ")
            << imp("[m]") << dim(" mic test  |  ")
            << imp("[q]") << dim(" done") << "\n\n";

        const QString in = readPromptLine(out, "  " + strong("Select") + ": ").toLower();
        if (in.isEmpty() || in == "q" || in == "quit" || in == "exit") break;

        if (in == "1") interactiveHudThemeSetup(b, out, reloadDaemon);
        else if (in == "2") flip("doubleTapLock", b.doubleTapLock());
        else if (in == "3") interactiveLanguageSetup(b, out, reloadDaemon);
        else if (in == "4") flip("whisperMode", b.whisperMode());
        else if (in == "5") flip("soundEffects", b.soundEffects());
        else if (in == "6") flip("pauseMedia", b.pauseMedia());
        else if (in == "7") {
            b.refreshMicrophones();
            const auto mics = b.microphones();
            printSection(out, "Select Microphone");
            for (int i = 0; i < mics.size(); ++i) {
                const auto m = mics[i].toMap();
                const bool cur = m["id"].toString() == b.microphone();
                out << "  " << imp(QString("[%1]").arg(i + 1)) << " "
                    << (cur ? good("* ") : "  ") << strong(m["label"].toString())
                    << "  " << dim(m["id"].toString()) << "\n";
            }
            const QString pick = readPromptLine(out, "\n  " + strong("Microphone #") + ": ");
            const int n = pick.toInt();
            if (n >= 1 && n <= mics.size()) {
                const QString id = mics[n - 1].toMap()["id"].toString();
                b.setOption("microphone", id);
                if (reloadDaemon) reloadDaemon();
                out << "\n  " << good("+") << " Microphone -> " << imp(id) << "\n";
            }
        } else if (in == "8") cycle("cleanup", {"none", "light", "medium"}, b.cleanup());
        else if (in == "9") cycle("style", {"auto", "formal", "casual", "lowercase", "excited", "verbatim"}, b.style());
        else if (in == "10") cycle("codeMode", {"auto", "on", "off"}, b.codeMode());
        else if (in == "11") flip("fileTagging", b.fileTagging());
        else if (in == "12") flip("autoInsert", b.autoInsert());
        else if (in == "13") flip("restoreClipboard", b.restoreClipboard());
        else if (in == "14") flip("contextAware", b.contextAware());
        else if (in == "15") flip("keepHistory", b.keepHistory());
        else if (in == "16") flip("autostart", b.autostart());
        else if (in == "s") interactiveShortcutSetup(b, d, out, reloadDaemon, portalBind);
        else if (in == "m") {
            QTextStream err(stderr);
            runMicTest(b.microphone(), out, err);
        } else {
            out << "\n  " << warn("!") << " Unknown option: " << in << "\n";
        }
    }
    out << "\n";
}

int runMicTest(const QString &microphone, QTextStream &out, QTextStream &err)
{
    QString program;
    QStringList args;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const bool pulse = qEnvironmentVariableIsSet("PULSE_SERVER")
        || qEnvironmentVariableIsSet("PULSE_SOURCE") || QFileInfo::exists(runtime + "/pulse/native");
    const QString parec = QStandardPaths::findExecutable("parec");
    const QString pw = QStandardPaths::findExecutable("pw-record");
    const QString arec = QStandardPaths::findExecutable("arecord");
    const QString mic = (microphone == "default" || microphone == "auto") ? QString() : microphone;

    if (!parec.isEmpty() && (pulse || (pw.isEmpty() && arec.isEmpty()))) {
        program = parec;
        args = {"--raw", "--format=s16le", "--rate=16000", "--channels=1", "--latency-msec=30"};
        if (!mic.isEmpty()) args << ("--device=" + mic);
    } else if (!pw.isEmpty()) {
        program = pw;
        args = {"--raw", "--format=s16", "--rate=16000", "--channels=1", "--latency=30ms"};
        if (!mic.isEmpty()) args << ("--target=" + mic);
        args << "-";
    } else if (!arec.isEmpty()) {
        program = arec;
        args = {"-q", "-t", "raw", "-f", "S16_LE", "-r", "16000", "-c", "1"};
        if (!mic.isEmpty()) args << "-D" << mic;
    } else {
        err << bad("[ERR]") << " No microphone capture tool installed (parec, pw-record, or arecord).\n";
        return 1;
    }

    printBanner(out, "mic test");
    out << "\n  " << imp(">") << " Source: " << strong(mic.isEmpty() ? "System default" : mic)
        << "   " << dim("Speak into your mic -- press Enter to stop.") << "\n\n" << Qt::flush;

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(1500)) {
        err << bad("[ERR]") << " Failed to start " << program << "\n";
        return 1;
    }

    int samples = 0;
    while (proc.state() == QProcess::Running) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0) {
            char discard[64];
            if (::read(STDIN_FILENO, discard, sizeof(discard)) >= 0) break;
        }
        if (!proc.waitForReadyRead(60)) continue;
        const QByteArray chunk = proc.readAllStandardOutput();
        if (chunk.size() < 2) continue;
        double sq = 0;
        for (qsizetype i = 0; i + 1 < chunk.size(); i += 2) {
            const double s = qFromLittleEndian<qint16>(chunk.constData() + i) / 32768.0;
            sq += s * s;
        }
        const double rms = std::sqrt(sq / (chunk.size() / 2));
        const double db = 20.0 * std::log10(std::max(1e-6, rms));
        const double norm = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        const int bars = static_cast<int>(std::round(norm * 32));
        QString bar = QString(bars, '#') + QString(32 - bars, '.');
        QString coloredBar = norm > 0.85 ? bad(bar) : norm > 0.35 ? good(bar) : norm > 0.12 ? imp(bar) : dim(bar);
        QString status = norm > 0.85 ? bad("HOT ") : norm > 0.25 ? good("GOOD") : norm > 0.10 ? imp("LOW ") : dim("IDLE");
        out << "\r  [" << coloredBar << "]  " << status << " " << dim(QString::number(db, 'f', 0) + " dB   ") << Qt::flush;
        if (++samples > 600 && !isatty(STDIN_FILENO)) break;
    }
    proc.terminate();
    if (!proc.waitForFinished(500)) proc.kill();
    out << "\n\n  " << good("+") << " Microphone test ended.\n\n";
    return 0;
}

} // namespace CliUi
