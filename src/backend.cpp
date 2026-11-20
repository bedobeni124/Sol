#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool isEntry(const QString &kind) { return kind == "dictionary" || kind == "snippets"; }

int find(const QJsonArray &rows, const QString &key, const QString &value)
{
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].toObject()[key].toString().compare(value, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

int countWords(const QString &text)
{
    static const QRegularExpression word(R"([\p{L}\p{N}]+(?:['’-][\p{L}\p{N}]+)*)");
    int n = 0;
    auto it = word.globalMatch(text);
    while (it.hasNext()) { it.next(); ++n; }
    return n;
}
} // namespace

Backend::Backend(bool interactive, QObject *parent, const QString &dataDirOverride)
    : QObject(parent), gui(interactive)
{
    const QString dir = dataDirOverride.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sol"
        : dataDirOverride;
    QDir().mkpath(dir);
    storage = dir + "/personal.json";
    if (!dataDirOverride.isEmpty())
        autostartFileOverride = dir + "/org.sol.Dictation.desktop";

    QFile f(storage);
    if (f.exists()) {
        auto doc = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
        if (doc.isObject()) {
            data = doc.object();
            const auto rows = data["history"].toArray();
            if (!rows.isEmpty()) {
                latest = rows.first().toObject()["text"].toString();
                entryId = rows.first().toObject()["id"].toString();
            }
        } else {
            corrupt = true;
            notice = tr("Could not read %1. Restore or move the file before saving changes.").arg(storage);
        }
    }

    doubleTapTimer.setSingleShot(true);
    doubleTapTimer.setInterval(300);
    connect(&doubleTapTimer, &QTimer::timeout, this, &Backend::stopNow);

    hudToastTimer.setSingleShot(true);
    connect(&hudToastTimer, &QTimer::timeout, this, [this] {
        hudToast.clear();
        pushHud();
    });

    connect(&speech, &SpeechEngine::recordingStarted, this, [this] {
        timer.start();
        setPhase("recording");
        if (soundEffects()) audio.play(mode == "handsfree" ? AudioFeedback::Cue::Lock : AudioFeedback::Cue::Start);
    });
    connect(&speech, &SpeechEngine::processingStarted, this, [this] {
        setPhase("processing");
        if (pauseMedia()) audio.resumeMedia();
    });
    connect(&speech, &SpeechEngine::readyChanged, this, &Backend::changed);
    connect(&speech, &SpeechEngine::levelChanged, this, [this](qreal v) {
        rms = v;
        emit volumeChanged();
        if (external && phase == "recording") pushHud();
    });
    connect(&speech, &SpeechEngine::failed, this, &Backend::fail);
    connect(&speech, &SpeechEngine::result, this, [this](const QString &text) {
        raw = text;
        detected = speech.detectedLanguage();
        FormatOptions opts;
        opts.language = detected.isEmpty() ? language() : detected;
        opts.cleanup = overrideActive && !cleanupOverrideValue.isEmpty() ? cleanupOverrideValue : cleanup();
        opts.style = overrideActive && !styleOverrideValue.isEmpty() ? styleOverrideValue : appStyle();
        opts.codeMode = isCodeContext(target);
        opts.fileTagging = fileTagging();
        opts.noTrailingPeriod = isUrlBarContext(target);
        opts.contextSymbols = contextSymbols();
        opts.dictionary = dictionary();
        opts.snippets = snippets();
        if (gui && QGuiApplication::clipboard())
            opts.clipboardText = QGuiApplication::clipboard()->text();
        finish(Formatter::format(raw, opts));
    });

    if (gui) {
        connect(&desktop, &Desktop::action, this, &Backend::control);
        connect(&desktop, &Desktop::hudMessage, this, &Backend::handleHudMessage);
        connect(&desktop, &Desktop::shortcutsChanged, this, &Backend::shortcutChanged);
        desktop.initialize(shortcut());
        refreshMicrophones();
        QTimer::singleShot(0, this, [this] { speech.warmUp(); });
    }
}

bool Backend::save(const QJsonObject &next)
{
    if (corrupt) {
        say(tr("Cannot save until %1 is restored or moved.").arg(storage));
        return false;
    }
    QSaveFile file(storage);
    const auto bytes = QJsonDocument(next).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        say(tr("Could not save Sol data: %1").arg(file.errorString()));
        return false;
    }
    data = next;
    emit settingsChanged();
    return true;
}

void Backend::say(const QString &text)
{
    notice = text;
    emit changed();
}

void Backend::pushHud(const QString &toast, const QString &toastKind)
{
    if (!gui || hudEditing) return;
    const QString shownToast = toast.isEmpty() ? hudToast : toast;
    const QString shownKind = toast.isEmpty() ? hudToastKind : toastKind;
    desktop.indicate({
        {"phase", (external || phase != "idle") ? phase : QStringLiteral("idle")},
        {"mode", mode},
        {"hud", showHud()},
        {"theme", hudTheme()},
        {"level", rms},
        {"whisper", whisperMode()},
        {"toast", shownToast},
        {"toastKind", shownKind},
    });
}

void Backend::showHudToast(const QString &toast, const QString &kind, int ms)
{
    if (!gui || toast.isEmpty() || hudEditing) return;
    hudToast = toast;
    hudToastKind = kind;
    pushHud();
    if (!hudHovered) hudToastTimer.start(ms);
}

void Backend::setPhase(const QString &value)
{
    phase = value;
    if (phase != "recording") {
        rms = 0;
        emit volumeChanged();
    }
    if (external || phase != "idle")
        pushHud();
    emit changed();
}

bool Backend::showHud() const
{
    if (data.contains("showHud")) return data.value("showHud").toBool(true);
    return data.value("flowBarMode").toString("auto") != "hidden";
}

QString Backend::hudTheme() const
{
    const QString t = data.value("hudTheme").toString("glass").trimmed().toLower();
    return t == "mono" ? QStringLiteral("mono") : QStringLiteral("glass");
}

QString Backend::shortcut() const
{
    const QString s = data.value("shortcut").toString();
    if (!s.isEmpty()) return s;
    const QString legacy = data.value("x11Hold").toString();
    return legacy.isEmpty() ? QStringLiteral("F9") : legacy;
}

QStringList Backend::preferredLanguages() const
{
    QStringList list;
    for (const auto &v : data.value("preferredLanguages").toArray()) {
        const QString s = v.toString().trimmed().toLower();
        if (!s.isEmpty() && !list.contains(s)) list.append(s);
    }
    return list;
}

QString Backend::languagesDisplay() const
{
    const QString lang = language();
    const QStringList pref = preferredLanguages();
    if (lang != "auto") return lang;
    if (!pref.isEmpty()) return "auto (" + pref.join(", ") + ")";
    return "auto (all)";
}

int Backend::totalWords() const
{
    const auto s = data.value("stats").toObject();
    if (s.contains("words")) return s.value("words").toInt();
    int sum = 0;
    for (const auto &v : data.value("history").toArray())
        sum += countWords(v.toObject().value("text").toString());
    return sum;
}

int Backend::totalDictations() const
{
    const auto s = data.value("stats").toObject();
    if (s.contains("dictations")) return s.value("dictations").toInt();
    return data.value("history").toArray().size();
}

int Backend::averageWpm() const
{
    const auto s = data.value("stats").toObject();
    const double sec = s.value("timedSeconds").toDouble();
    const int timedWords = s.value("timedWords").toInt();
    if (sec >= 1.0 && timedWords > 0)
        return qBound(1, static_cast<int>(std::round((timedWords * 60.0) / sec)), 400);
    double hSec = 0;
    int hWords = 0;
    for (const auto &v : data.value("history").toArray()) {
        const auto row = v.toObject();
        const double d = row.value("duration").toDouble();
        if (d >= 0.8) {
            hSec += d;
            hWords += countWords(row.value("text").toString());
        }
    }
    return (hSec >= 1.0 && hWords > 0)
        ? qBound(1, static_cast<int>(std::round((hWords * 60.0) / hSec)), 400)
        : 0;
}

int Backend::streakDays() const
{
    QSet<QDate> activeDates;
    for (const auto &v : data.value("stats").toObject().value("days").toArray()) {
        const QDate d = QDate::fromString(v.toString(), Qt::ISODate);
        if (d.isValid()) activeDates.insert(d);
    }
    for (const auto &v : data.value("history").toArray()) {
        const QDate d = QDateTime::fromString(v.toObject().value("created").toString(), Qt::ISODateWithMs).date();
        if (d.isValid()) activeDates.insert(d);
    }
    if (activeDates.isEmpty()) return 0;
    QDate cursor = QDate::currentDate();
    if (!activeDates.contains(cursor)) cursor = cursor.addDays(-1);
    int streak = 0;
    while (activeDates.contains(cursor)) {
        ++streak;
        cursor = cursor.addDays(-1);
    }
    return streak;
}

int Backend::minutesSaved() const
{
    const int words = totalWords();
    if (words <= 0) return 0;
    const int wpm = averageWpm() > 50 ? averageWpm() : 145;
    return std::max(0, static_cast<int>(std::round(words / 45.0 - words / static_cast<double>(wpm))));
}

void Backend::recordStats(QJsonObject &next, const QString &text, qreal durationSec) const
{
    const int words = countWords(text);
    if (words <= 0) return;
    QJsonObject stats = next.value("stats").toObject();
    if (!stats.contains("words")) stats["words"] = totalWords();
    if (!stats.contains("dictations")) stats["dictations"] = totalDictations();
    stats["words"] = stats.value("words").toInt() + words;
    stats["dictations"] = stats.value("dictations").toInt() + 1;
    if (durationSec >= 0.7) {
        stats["timedWords"] = stats.value("timedWords").toInt() + words;
        stats["timedSeconds"] = stats.value("timedSeconds").toDouble() + durationSec;
    }
    QJsonArray days = stats.value("days").toArray();
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    bool hasToday = false;
    for (const auto &d : days) if (d.toString() == today) { hasToday = true; break; }
    if (!hasToday) {
        days.append(today);
        while (days.size() > 400) days.removeFirst();
        stats["days"] = days;
    }
    next["stats"] = stats;
}

QString Backend::autostartPath() const
{
    if (!autostartFileOverride.isEmpty()) return autostartFileOverride;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/autostart/org.sol.Dictation.desktop";
}

bool Backend::autostart() const
{
    return QFileInfo::exists(autostartPath());
}

bool Backend::writeAutostart(bool enable)
{
    const QString path = autostartPath();
    if (!enable) return !QFileInfo::exists(path) || QFile::remove(path);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString exec = QStandardPaths::findExecutable("sol");
    if (exec.isEmpty()) exec = QCoreApplication::applicationFilePath();
    const QByteArray desktopEntry = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Sol\n"
        "Comment=Local speech-to-text dictation\n"
        "Exec=%1 daemon\n"
        "Icon=org.sol.Dictation\n"
        "Terminal=false\n"
        "Categories=Utility;Accessibility;\n"
        "X-GNOME-Autostart-enabled=true\n").arg(exec).toUtf8();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(desktopEntry) == desktopEntry.size() && file.commit();
}

QVariant Backend::optionValue(const QString &key) const
{
    if (key == "language") return language();
    if (key == "preferredLanguages") return preferredLanguages().join(',');
    if (key == "cleanup") return cleanup();
    if (key == "style") return style();
    if (key == "personalStyle") return personalStyle();
    if (key == "workStyle") return workStyle();
    if (key == "emailStyle") return emailStyle();
    if (key == "codeStyle") return codeStyle();
    if (key == "otherStyle") return otherStyle();
    if (key == "codeMode") return codeMode();
    if (key == "fileTagging") return fileTagging();
    if (key == "whisperMode") return whisperMode();
    if (key == "microphone") return microphone();
    if (key == "soundEffects") return soundEffects();
    if (key == "pauseMedia") return pauseMedia();
    if (key == "restoreClipboard") return restoreClipboard();
    if (key == "doubleTapLock") return doubleTapLock();
    if (key == "showHud") return showHud();
    if (key == "hudTheme") return hudTheme();
    if (key == "autoInsert") return autoInsert();
    if (key == "contextAware") return contextAware();
    if (key == "keepHistory") return keepHistory();
    if (key == "autostart") return autostart();
    if (key == "shortcut") return shortcut();
    return {};
}

bool Backend::setLanguages(const QString &lang, const QStringList &preferred)
{
    const QString cleanLang = lang.trimmed().toLower();
    static const QRegularExpression langRx("^(auto|[a-z]{2,3})$");
    if (!langRx.match(cleanLang).hasMatch()) return false;
    QJsonArray arr;
    QStringList seen;
    for (const QString &rawCode : preferred) {
        const QString c = rawCode.trimmed().toLower();
        if (c != "auto" && langRx.match(c).hasMatch() && !seen.contains(c) && seen.size() < 6) {
            seen.append(c);
            arr.append(c);
        }
    }
    QJsonObject next = data;
    if (cleanLang == "auto" && seen.size() == 1) {
        next["language"] = seen.first();
        next["preferredLanguages"] = QJsonArray();
    } else {
        next["language"] = cleanLang;
        next["preferredLanguages"] = (cleanLang == "auto") ? arr : QJsonArray();
    }
    return save(next);
}

void Backend::setOption(const QString &key, const QVariant &v)
{
    static const QStringList styles = {"formal", "casual", "lowercase", "excited", "verbatim"};
    if (key == "autostart") {
        if (!writeAutostart(v.toBool()))
            say(tr("Could not update login autostart setting."));
        emit settingsChanged();
        return;
    }
    if (key == "preferredLanguages") {
        const QString rawVal = v.toString().trimmed().toLower();
        if (rawVal.isEmpty() || rawVal == "auto" || rawVal == "all") {
            setLanguages("auto", {});
        } else {
            const QStringList parts = rawVal.split(QRegularExpression(R"([,\s]+)"), Qt::SkipEmptyParts);
            setLanguages(parts.size() == 1 ? parts.first() : QStringLiteral("auto"), parts);
        }
        return;
    }
    QJsonObject next = data;
    if (QStringList{"keepHistory", "contextAware", "autoInsert", "whisperMode", "soundEffects",
                    "pauseMedia", "restoreClipboard", "doubleTapLock", "fileTagging", "showHud"}.contains(key)) {
        next[key] = v.toBool();
    } else if (key == "cleanup" && QStringList{"none", "light", "medium"}.contains(v.toString())) {
        next[key] = v.toString();
    } else if (key == "codeMode" && QStringList{"auto", "on", "off"}.contains(v.toString())) {
        next[key] = v.toString();
    } else if (key == "hudTheme" && QStringList{"glass", "mono"}.contains(v.toString().trimmed().toLower())) {
        next["hudTheme"] = v.toString().trimmed().toLower();
    } else if (key == "microphone") {
        next[key] = v.toString().trimmed().isEmpty() ? QStringLiteral("default") : v.toString().trimmed();
    } else if (key == "language" && QRegularExpression("^(auto|[a-z]{2,3})$").match(v.toString().trimmed().toLower()).hasMatch()) {
        const QString c = v.toString().trimmed().toLower();
        next["language"] = c;
        if (c != "auto") next["preferredLanguages"] = QJsonArray();
    } else if (QStringList{"style", "personalStyle", "workStyle", "emailStyle", "codeStyle", "otherStyle"}.contains(key)
               && (styles.contains(v.toString()) || (key == "style" && v == "auto"))) {
        next[key] = v.toString();
    } else if (key == "shortcut" || key == "x11Hold") {
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) return;
        next["shortcut"] = s;
        next["x11Hold"] = s;
    } else {
        return;
    }

    if (save(next)) {
        if (key == "contextAware" && !contextAware()) {
            priorTarget = {};
            priorText.clear();
            target.remove("before");
            target.remove("after");
            target.remove("title");
            target.remove("files");
        }
        if (key == "showHud" || key == "hudTheme") pushHud();
        if (key == "shortcut" || key == "x11Hold") desktop.rebindX11(shortcut());
    }
}

void Backend::toggleOption(const QString &key)
{
    if (key == "whisperMode") setOption(key, !whisperMode());
    else if (key == "soundEffects") setOption(key, !soundEffects());
    else if (key == "pauseMedia") setOption(key, !pauseMedia());
    else if (key == "autostart") setOption(key, !autostart());
    else if (key == "restoreClipboard") setOption(key, !restoreClipboard());
    else if (key == "doubleTapLock") setOption(key, !doubleTapLock());
    else if (key == "showHud") setOption(key, !showHud());
    else if (key == "fileTagging") setOption(key, !fileTagging());
    else if (key == "keepHistory") setOption(key, !keepHistory());
    else if (key == "autoInsert") setOption(key, !autoInsert());
    else if (key == "contextAware") setOption(key, !contextAware());
}

bool Backend::addAppRule(const QString &appPattern, const QString &styleValue)
{
    static const QStringList styles = {"formal", "casual", "lowercase", "excited", "verbatim"};
    const QString pat = appPattern.simplified().toLower();
    if (pat.isEmpty() || !styles.contains(styleValue)) return false;
    QJsonArray rules = data["appRules"].toArray();
    const int idx = find(rules, "app", pat);
    QJsonObject row{{"app", pat}, {"style", styleValue}};
    if (idx < 0) rules.prepend(row);
    else rules[idx] = row;
    QJsonObject next = data;
    next["appRules"] = rules;
    return save(next);
}

bool Backend::removeAppRule(const QString &appPattern)
{
    QJsonArray rules = data["appRules"].toArray();
    const int idx = find(rules, "app", appPattern.simplified());
    if (idx < 0) return false;
    rules.removeAt(idx);
    QJsonObject next = data;
    next["appRules"] = rules;
    return save(next);
}

bool Backend::addEntry(const QString &kind, const QString &trigger, const QString &value)
{
    const QString name = trigger.simplified(), expansion = value.trimmed();
    if (!isEntry(kind) || name.isEmpty() || (expansion.isEmpty() && kind == "snippets")) {
        say(tr("Enter a spoken phrase and its expansion."));
        return false;
    }
    const QString other = kind == "dictionary" ? "snippets" : "dictionary";
    if (find(data[other].toArray(), "trigger", name) >= 0) {
        say(tr("That phrase already belongs to another entry."));
        return false;
    }
    QJsonArray rows = data[kind].toArray();
    const auto pos = find(rows, "trigger", name);
    QJsonObject row{{"trigger", name}, {"value", expansion.isEmpty() ? name : expansion}};
    if (pos < 0) rows.prepend(row);
    else rows[pos] = row;
    auto next = data;
    next[kind] = rows;
    return save(next);
}

bool Backend::removeEntry(const QString &kind, const QString &trigger)
{
    if (!isEntry(kind)) return false;
    auto rows = data[kind].toArray();
    const int i = find(rows, "trigger", trigger);
    if (i < 0) return false;
    rows.removeAt(i);
    auto next = data;
    next[kind] = rows;
    return save(next);
}

bool Backend::learn(QJsonObject &next, const QString &oldText, const QString &newText) const
{
    static const QRegularExpression word("[\\p{L}\\p{M}]+(?:[-'’][\\p{L}\\p{M}]+)*");
    auto a = word.globalMatch(oldText), b = word.globalMatch(newText);
    QString was, now;
    while (a.hasNext() && b.hasNext()) {
        auto left = a.next().captured(), right = b.next().captured();
        if (left == right) continue;
        if (!was.isEmpty()) return false;
        was = left;
        now = right;
    }
    if (a.hasNext() || b.hasNext() || was.size() < 2 || now.size() < 2
        || find(next["snippets"].toArray(), "trigger", was) >= 0) return false;
    auto rows = next["dictionary"].toArray();
    if (find(rows, "trigger", was) >= 0) return false;
    rows.prepend(QJsonObject{{"trigger", was}, {"value", now}, {"autoLearned", true}});
    next["dictionary"] = rows;
    return true;
}

bool Backend::editHistory(const QString &id, const QString &text)
{
    if (text.trimmed().isEmpty()) return false;
    auto rows = data["history"].toArray();
    const int i = find(rows, "id", id);
    if (i < 0) return false;
    auto next = data, row = rows[i].toObject();
    bool learned = learn(next, row["text"].toString(), text);
    row["text"] = text;
    rows[i] = row;
    next["history"] = rows;
    if (!save(next)) return false;
    if (id == entryId) latest = text;
    say(learned ? tr("Correction saved to your dictionary.") : tr("Dictation updated."));
    return true;
}

bool Backend::revertToRaw(const QString &id)
{
    auto rows = data["history"].toArray();
    const int i = find(rows, "id", id);
    if (i < 0) return false;
    auto row = rows[i].toObject();
    const QString original = row["raw"].toString().trimmed();
    if (original.isEmpty()) return false;
    row["text"] = original;
    rows[i] = row;
    auto next = data;
    next["history"] = rows;
    if (!save(next)) return false;
    if (id == entryId) latest = original;
    say(tr("Reverted to original transcription."));
    return true;
}

bool Backend::reformatHistory(const QString &id, const QString &targetStyle)
{
    auto rows = data["history"].toArray();
    const int i = find(rows, "id", id);
    if (i < 0) return false;
    auto row = rows[i].toObject();
    const QString source = row["raw"].toString().trimmed().isEmpty()
        ? row["text"].toString() : row["raw"].toString();
    FormatOptions opts;
    opts.language = row["language"].toString("en");
    opts.cleanup = targetStyle == "verbatim" ? "none" : "light";
    opts.style = targetStyle;
    opts.dictionary = dictionary();
    opts.snippets = snippets();
    const QString updated = Formatter::format(source, opts);
    if (updated.isEmpty()) return false;
    row["text"] = updated;
    rows[i] = row;
    auto next = data;
    next["history"] = rows;
    if (!save(next)) return false;
    if (id == entryId) latest = updated;
    say(tr("Reformatted dictation."));
    return true;
}

void Backend::removeHistory(const QString &id)
{
    auto rows = data["history"].toArray();
    const int i = find(rows, "id", id);
    if (i < 0) return;
    rows.removeAt(i);
    auto next = data;
    next["history"] = rows;
    if (save(next) && id == entryId) {
        latest.clear();
        entryId.clear();
        emit changed();
    }
}

void Backend::clearHistory()
{
    auto next = data;
    next["history"] = QJsonArray();
    if (save(next)) {
        latest.clear();
        entryId.clear();
        priorTarget = {};
        priorText.clear();
        emit changed();
    }
}

void Backend::refreshMicrophones()
{
    micList = SpeechEngine::listMicrophones();
    emit microphonesChanged();
}

bool Backend::isCodeContext(const QJsonObject &focusTarget) const
{
    if (codeMode() == "on") return true;
    if (codeMode() == "off") return false;
    const QString where = (focusTarget["app"].toString() + " " + focusTarget["title"].toString()).toLower();
    static const QRegularExpression codeRx(
        R"(code|cursor|windsurf|zed|nvim|neovim|vim|emacs|jetbrains|idea|pycharm|clion|webstorm|goland|rustrover|sublime|kate|antigravity|claude|github|gitlab|term|kitty|konsole|ghostty|alacritty|tilix|foot|ptyxis|contour|wezterm|warp|urxvt|rxvt)");
    return codeRx.match(where).hasMatch();
}

bool Backend::isUrlBarContext(const QJsonObject &focusTarget) const
{
    if (focusTarget.value("urlBar").toBool()) return true;
    const QString before = focusTarget.value("before").toString().trimmed();
    static const QRegularExpression urlPrefixRx(
        R"(^(?:https?://|www\.|about:|chrome://|brave://|edge://|vivaldi://|file://|localhost[:/]))",
        QRegularExpression::CaseInsensitiveOption);
    if (!before.isEmpty() && urlPrefixRx.match(before).hasMatch()) return true;
    const QString app = focusTarget.value("app").toString().toLower();
    static const QRegularExpression launcherRx(
        R"(walker|rofi|wofi|fuzzel|dmenu|tofi|vicinae|albert|ulauncher|krunner|anyrun)");
    if (!app.isEmpty() && launcherRx.match(app).hasMatch()) return true;

    // On Chromium-based browsers without AT-SPI2 enabled (e.g. Brave, Helium, Chrome),
    // a New Tab / Start Page window always focuses the address bar.
    static const QRegularExpression browserRx(
        R"(brave|chromium|chrome|helium|firefox|zen|vivaldi|opera|edge|librewolf|floorp|epiphany|qutebrowser|waterfox|thorium)");
    static const QRegularExpression newTabRx(
        R"(^(?:new\s+tab|start\s+page|speed\s+dial|blank\s+page|about:blank|új\s+lap|neuer\s+tab|nouvel\s+onglet)\b)",
        QRegularExpression::CaseInsensitiveOption);
    const QString title = focusTarget.value("title").toString().trimmed();
    return !app.isEmpty() && browserRx.match(app).hasMatch() && newTabRx.match(title).hasMatch();
}

QString Backend::resolveAppStyle(const QJsonObject &focusTarget) const
{
    if (style() != "auto") return style();
    const QString where = (focusTarget["app"].toString() + " " + focusTarget["title"].toString()).toLower();
    for (const auto &v : data["appRules"].toArray()) {
        const auto row = v.toObject();
        const QString pat = row["app"].toString().toLower();
        if (!pat.isEmpty() && where.contains(pat))
            return row["style"].toString("formal");
    }
    auto matches = [&](const QString &s) { return QRegularExpression(s).match(where).hasMatch(); };
    if (matches("gmail|thunderbird|outlook|evolution|kmail|geary|proton mail|fastmail|mailspring")) return emailStyle();
    if (matches("slack|microsoft teams|mattermost|zulip|linear|jira|notion|confluence")) return workStyle();
    if (matches("signal|telegram|whatsapp|discord|vesktop|beeper|messages|element")) return personalStyle();
    if (isCodeContext(focusTarget)) return codeStyle();
    return otherStyle();
}

QString Backend::appStyle() const
{
    return resolveAppStyle(target);
}

QStringList Backend::contextSymbols() const
{
    if (!contextAware()) return {};
    QStringList syms;
    for (const auto &f : target["files"].toArray()) {
        const QString name = f.toString();
        if (!name.isEmpty()) syms.append(name);
    }
    const QString combined = target["title"].toString() + "\n"
        + target["before"].toString() + "\n" + target["after"].toString();
    for (const QString &s : Formatter::extractSymbols(combined, 60)) {
        if (!syms.contains(s)) syms.append(s);
    }
    return syms;
}

QString Backend::prompt() const
{
    static const QRegularExpression hasWordChar(R"([\p{L}\p{N}])");
    QStringList hints;
    for (const auto &entry : dictionary()) {
        const QString val = entry.toMap().value("value").toString();
        if (hasWordChar.match(val).hasMatch()) hints << val;
    }
    for (const auto &entry : snippets()) hints << entry.toMap().value("trigger").toString();
    if (isCodeContext(target)) {
        const QStringList syms = contextSymbols();
        for (int i = 0; i < std::min<qsizetype>(syms.size(), 25); ++i)
            hints << syms[i];
    }
    const auto before = contextAware() ? target["before"].toString().right(160) : QString();
    return (before + " " + hints.join(", ").left(750)).trimmed();
}

QString Backend::matchContext(QString text) const
{
    if (!contextAware() || text.isEmpty() || !target.contains("before")) return text;
    const auto before = target["before"].toString(), after = target["after"].toString();
    bool spaced = !QStringList{"zh", "ja", "th", "lo", "my", "km"}.contains(detected);
    const auto line = before.section('\n', -1).trimmed();
    if (!line.isEmpty() && spaced) {
        const QString first = text.section(' ', 0, 0);
        bool keep = first == "I" || first.startsWith("I'") || first[0].isLower()
                    || (first.size() > 1 && first[1].isUpper());
        for (const auto &row : dictionary())
            if (row.toMap()["value"].toString().startsWith(first)) keep = true;
        for (const auto &row : snippets())
            if (text.startsWith(row.toMap()["value"].toString())) keep = true;
        if (cleanup() != "none" && !keep && !QString(".!?。！？").contains(line.back()))
            text[0] = text[0].toLower();
        if (!before.back().isSpace() && !QString("([/-").contains(before.back()) && text[0].isLetterOrNumber())
            text.prepend(' ');
    }
    if (!after.isEmpty() && after[0].isLetterOrNumber() && spaced) {
        if (text.endsWith('.') && !text.endsWith("...")) text.chop(1);
        text += ' ';
    }
    return text;
}

QString Backend::previewStyle(const QString &sample, const QString &tone) const
{
    FormatOptions opts;
    opts.language = "en";
    opts.cleanup = tone == "verbatim" ? "none" : "light";
    opts.style = tone;
    return Formatter::format(sample, opts);
}

void Backend::begin(bool fromShortcut, const QString &requestedMode)
{
    if (phase != "idle" || writing) return;
    ++generation;
    pendingStart = false;
    const bool wasEditing = hudEditing;
    hudEditing = false;
    hudHovered = false;
    target = {};
    duration = 0;
    external = fromShortcut;
    mode = requestedMode;
    overrideActive = false;
    styleOverrideValue.clear();
    cleanupOverrideValue.clear();
    notice.clear();
    raw.clear();
    hudToast.clear();
    hudToastTimer.stop();
    timer.invalidate();
    pressTimer.start();
    if (wasEditing) {
        desktop.indicate({{"editing", false}, {"phase", "starting"}, {"mode", mode}, {"hud", showHud()}, {"theme", hudTheme()}});
    }
    setPhase("starting");

    if (pauseMedia() && gui) audio.pauseMedia();

    if (external) {
        auto serial = generation;
        const bool wantCode = (codeMode() != "off") && contextAware();
        desktop.request({
            {"op", "focus"},
            {"context", contextAware()},
            {"codeContext", wantCode},
        }, [this, serial](QJsonObject field) {
            if (serial != generation || phase == "idle") return;
            target = field;
            if (field["password"].toBool()) {
                cancel();
                say(tr("Password field detected. Dictation cancelled."));
                showHudToast(tr("Password field blocked"), "error", 2000);
                desktop.notify(notice);
                return;
            }
            if (contextAware() && !priorText.isEmpty() && priorTarget.contains("field")
                && field["field"] == priorTarget["field"] && field["after"] == priorTarget["after"]) {
                auto current = field["before"].toString();
                auto prefix = priorTarget["before"].toString();
                if (current.startsWith(prefix)) {
                    auto next = data;
                    if (learn(next, priorText, current.mid(prefix.size()))) save(next);
                }
            }
            priorText.clear();
            priorTarget = {};
            if (phase == "starting" || phase == "recording") speech.setPrompt(prompt());
        });
    }
    speech.start(language(), preferredLanguages(), prompt(), microphone(), whisperMode());
}

void Backend::stop()
{
    if (phase != "recording" && phase != "starting") return;
    if (mode == "ptt" && doubleTapLock() && pressTimer.isValid() && pressTimer.elapsed() < 280) {
        doubleTapTimer.start();
        return;
    }
    stopNow();
}

void Backend::stopNow()
{
    doubleTapTimer.stop();
    if (phase != "recording" && phase != "starting") return;
    duration = timer.isValid() ? timer.elapsed() / 1000.0 : 0;
    if (soundEffects()) audio.play(AudioFeedback::Cue::Stop);
    speech.stop();
}

void Backend::toggle()
{
    if (phase == "idle") begin(false, "handsfree");
    else stopNow();
}

void Backend::pasteLast()
{
    if (phase != "idle" || writing) return;
    QString text = latest;
    if (text.isEmpty() && !history().isEmpty())
        text = history().first().toMap().value("text").toString();
    if (text.isEmpty()) {
        say(tr("No previous dictation to paste."));
        showHudToast(tr("Nothing to paste"), "info", 1400);
        return;
    }
    writing = true;
    desktop.request({
        {"op", "paste_text"},
        {"text", text},
        {"restoreClipboard", restoreClipboard()},
    }, [this](QJsonObject answer) {
        writing = false;
        const QString reason = answer["notice"].toString();
        if (reason.isEmpty()) {
            if (soundEffects()) audio.play(AudioFeedback::Cue::Done);
            showHudToast(tr("Pasted"), "ok", 1000);
            say(tr("Pasted last dictation."));
        } else {
            say(reason);
            showHudToast(reason, "error", 2200);
            desktop.notify(reason);
        }
        maybeStartPending();
    });
}

void Backend::retryLast()
{
    if (phase != "idle" || !speech.hasLastRecording()) return;
    ++generation;
    external = false;
    mode = "handsfree";
    overrideActive = false;
    notice.clear();
    setPhase("processing");
    speech.retranscribeLast(language(), preferredLanguages(), prompt(), whisperMode());
}

void Backend::reloadFromDisk()
{
    QFile f(storage);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    corrupt = false;
    data = doc.object();
    const auto rows = data["history"].toArray();
    if (!rows.isEmpty()) {
        latest = rows.first().toObject()["text"].toString();
        entryId = rows.first().toObject()["id"].toString();
    } else {
        latest.clear();
        entryId.clear();
    }
    if (gui) {
        desktop.rebindX11(shortcut());
        pushHud();
    }
    emit settingsChanged();
    emit changed();
}

void Backend::control(const QString &action)
{
    if (action == "start") {
        // Second quick tap within double-tap window locks recording without holding the key.
        if (doubleTapTimer.isActive() && (phase == "recording" || phase == "starting")) {
            doubleTapTimer.stop();
            mode = "handsfree";
            if (soundEffects()) audio.play(AudioFeedback::Cue::Lock);
            pushHud();
            emit changed();
            return;
        }
        // Tapping again while locked stops and pastes immediately.
        if ((phase == "recording" || phase == "starting") && mode == "handsfree") {
            stopNow();
            return;
        }
        // If previous dictation is still finishing transcription/paste, queue immediate restart while held.
        if (phase == "processing" || writing) {
            pendingStart = true;
            return;
        }
        begin(true, "ptt");
    } else if (action == "stop") {
        pendingStart = false;
        if (mode == "handsfree") return;
        stop();
    } else if (action == "cancel") {
        pendingStart = false;
        cancel();
    } else if (action == "toggle") {
        if (phase == "idle" && !writing) begin(true, "handsfree");
        else if (phase == "recording" || phase == "starting") stopNow();
    } else if (action == "paste-last") {
        pasteLast();
    }
}

void Backend::cancel()
{
    doubleTapTimer.stop();
    pendingStart = false;
    if (phase == "idle" || writing) return;
    ++generation;
    speech.cancel();
    if (pauseMedia()) audio.resumeMedia();
    if (soundEffects()) audio.play(AudioFeedback::Cue::Cancel);
    const bool wasExternal = external;
    setPhase("idle");
    external = false;
    say(tr("Dictation cancelled."));
    if (wasExternal) showHudToast(tr("Cancelled"), "info", 900);
}

void Backend::transcribe(const QString &path, const QString &overrideLang,
                         const QString &overrideStyle, const QString &overrideCleanup)
{
    if (phase != "idle") return;
    external = false;
    mode = "handsfree";
    duration = 0;
    overrideActive = !overrideStyle.isEmpty() || !overrideCleanup.isEmpty();
    styleOverrideValue = overrideStyle;
    cleanupOverrideValue = overrideCleanup;
    setPhase("processing");
    speech.transcribeFile(path, overrideLang.isEmpty() ? language() : overrideLang,
                          preferredLanguages(), prompt(), whisperMode());
}

void Backend::maybeStartPending()
{
    if (pendingStart && phase == "idle" && !writing) {
        pendingStart = false;
        QTimer::singleShot(0, this, [this] { begin(true, "ptt"); });
    }
}

void Backend::fail(const QString &reason)
{
    doubleTapTimer.stop();
    ++generation;
    if (pauseMedia()) audio.resumeMedia();
    if (soundEffects()) audio.play(AudioFeedback::Cue::Cancel);
    const bool wasExternal = external;
    setPhase("idle");
    external = false;
    say(reason);
    if (wasExternal) {
        showHudToast(reason, "error", 2200);
        desktop.notify(reason);
    }
    emit completed({}, reason);
    maybeStartPending();
}

void Backend::handleHudMessage(const QJsonObject &msg)
{
    const QString act = msg.value("action").toString();
    if (act == "hud-hover") {
        hudHovered = msg.value("hover").toBool();
        if (hudHovered) {
            hudToastTimer.stop();
        } else if (!hudToast.isEmpty() && !hudEditing && phase == "idle") {
            hudToastTimer.start(900);
        }
        return;
    }
    if (act == "hud-edit-start") {
        doubleTapTimer.stop();
        hudToastTimer.stop();
        hudEditing = true;
        hudHovered = false;
        pendingStart = false;
        if (phase != "idle" && !writing) {
            ++generation;
            speech.cancel();
            if (pauseMedia()) audio.resumeMedia();
            phase = "idle";
            rms = 0;
            external = false;
            emit volumeChanged();
            emit changed();
        }
        return;
    }
    if (act == "hud-edit-cancel") {
        hudEditing = false;
        hudHovered = false;
        hudToast.clear();
        hudToastTimer.stop();
        pushHud();
        return;
    }
    if (act == "hud-learn") {
        hudEditing = false;
        hudHovered = false;
        const QString trig = msg.value("trigger").toString().simplified();
        const QString val = msg.value("value").toString().trimmed();
        if (addEntry("dictionary", trig, val)) {
            if (soundEffects()) audio.play(AudioFeedback::Cue::Done);
            const QString label = (trig == val)
                ? tr("Saved: %1").arg(val)
                : tr("Saved: %1 -> %2").arg(trig, val);
            showHudToast(label, "ok", 1700);
        } else {
            if (soundEffects()) audio.play(AudioFeedback::Cue::Cancel);
            showHudToast(notice.isEmpty() ? tr("Could not save rule") : notice, "error", 2200);
        }
        return;
    }
    if (act == "hud-forget") {
        hudEditing = false;
        hudHovered = false;
        const QString trig = msg.value("trigger").toString().simplified();
        if (removeEntry("dictionary", trig)) {
            if (soundEffects()) audio.play(AudioFeedback::Cue::Done);
            showHudToast(tr("Removed: %1").arg(trig), "ok", 1600);
        } else {
            if (soundEffects()) audio.play(AudioFeedback::Cue::Cancel);
            showHudToast(tr("Not found: %1").arg(trig), "error", 1800);
        }
        return;
    }
    if (!act.isEmpty()) control(act);
}

void Backend::finish(const QString &formatted)
{
    if (formatted.isEmpty()) {
        fail(tr("No text remained after cleanup."));
        return;
    }
    QString text = external ? matchContext(formatted) : formatted;
    latest = text.trimmed();
    entryId = uid();
    QStringList notes;

    if (gui && !latest.isEmpty()) {
        auto next = data;
        recordStats(next, latest, duration);
        if (keepHistory()) {
            auto rows = next["history"].toArray();
            rows.prepend(QJsonObject{
                {"id", entryId},
                {"text", latest},
                {"raw", raw},
                {"language", detected},
                {"app", target["app"].toString()},
                {"created", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                {"duration", duration},
            });
            while (rows.size() > 1500) rows.removeLast();
            next["history"] = rows;
        }
        if (!save(next)) notes << notice;
    }

    if (external && autoInsert() && !target["password"].toBool()) {
        writing = true;
        desktop.request({
            {"op", "deliver"},
            {"target", target},
            {"text", text},
            {"restoreClipboard", restoreClipboard()},
        }, [this, text, notes](QJsonObject answer) mutable {
            writing = false;
            const auto reason = answer["notice"].toString();
            if (!reason.isEmpty()) {
                notes << reason;
            } else {
                if (contextAware()) {
                    priorText = text;
                    priorTarget = target;
                }
                if (soundEffects()) audio.play(AudioFeedback::Cue::Done);
            }
            setPhase("idle");
            external = false;
            say(notes.join(" "));
            if (!reason.isEmpty()) {
                showHudToast(reason, "error", 2400);
                desktop.notify(reason);
            } else {
                showHudToast(tr("Pasted"), "ok", 2600);
            }
            emit completed(latest, {});
            maybeStartPending();
        });
    } else {
        if (soundEffects() && !latest.isEmpty()) audio.play(AudioFeedback::Cue::Done);
        setPhase("idle");
        external = false;
        say(notes.join(" "));
        emit completed(latest, {});
        maybeStartPending();
    }
}

QJsonObject Backend::status() const
{
    const QString activeKey = !desktop.hold.isEmpty() ? desktop.hold
        : (Desktop::hasActiveHyprlandBinds() ? shortcut() : QString());
    return {
        {"state", phase},
        {"mode", mode},
        {"speechReady", speechReady()},
        {"speechModel", speechModel()},
        {"speechDevice", speechDevice()},
        {"whisperMode", whisperMode()},
        {"showHud", showHud()},
        {"hudTheme", hudTheme()},
        {"message", notice},
        {"language", language()},
        {"languagesDisplay", languagesDisplay()},
        {"desktop", desktop.desktopEnvironment()},
        {"shortcut", activeKey},
        {"savedShortcut", shortcut()},
        {"shortcutActive", !activeKey.isEmpty()},
        {"totalWords", totalWords()},
        {"averageWpm", averageWpm()},
        {"streakDays", streakDays()},
    };
}
