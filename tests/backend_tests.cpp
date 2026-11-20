#include "../src/audiofeedback.h"
#include "../src/backend.h"
#include "../src/cli_ui.h"
#include "../src/desktop.h"
#include "../src/hudwindow.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QtEndian>

class BackendTests : public QObject {
    Q_OBJECT
private slots:
    void settingsAndPersistence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            Backend b(false, nullptr, dir.path());
            QCOMPARE(b.whisperMode(), false);
            QCOMPARE(b.soundEffects(), true);
            QCOMPARE(b.restoreClipboard(), true);
            QCOMPARE(b.doubleTapLock(), true);
            QCOMPARE(b.showHud(), true);
            QCOMPARE(b.hudTheme(), QString("glass"));
            QCOMPARE(b.codeMode(), QString("auto"));
            QCOMPARE(b.shortcut(), QString("F9"));

            b.setOption("whisperMode", true);
            b.setOption("pauseMedia", true);
            b.setOption("showHud", false);
            b.setOption("hudTheme", "mono");
            b.setOption("codeMode", "on");
            b.setOption("cleanup", "medium");
            b.setOption("shortcut", "Ctrl+Alt+Space");
            QVERIFY(b.setLanguages("auto", {"en", "hu"}));
            QCOMPARE(b.language(), QString("auto"));
            QCOMPARE(b.preferredLanguages(), (QStringList{"en", "hu"}));
            QCOMPARE(b.languagesDisplay(), QString("auto (en, hu)"));
            QCOMPARE(b.optionValue("shortcut").toString(), QString("Ctrl+Alt+Space"));
            QCOMPARE(b.optionValue("showHud").toBool(), false);
            QVERIFY(b.addAppRule("obsidian", "lowercase"));
            QVERIFY(b.addEntry("dictionary", "kube", "Kubernetes"));
            QVERIFY(b.addEntry("snippets", "my sig", "Best,\nAlex"));
            QVERIFY(!b.addEntry("snippets", "kube", "conflict"));

            b.setOption("autostart", true);
            QVERIFY(b.autostart());
            QVERIFY(QFile::exists(dir.filePath("org.sol.Dictation.desktop")));
        }

        Backend b2(false, nullptr, dir.path());
        QCOMPARE(b2.whisperMode(), true);
        QCOMPARE(b2.pauseMedia(), true);
        QCOMPARE(b2.showHud(), false);
        QCOMPARE(b2.hudTheme(), QString("mono"));
        QCOMPARE(b2.codeMode(), QString("on"));
        QCOMPARE(b2.shortcut(), QString("Ctrl+Alt+Space"));
        QCOMPARE(b2.preferredLanguages(), (QStringList{"en", "hu"}));
        QCOMPARE(b2.dictionary().size(), 1);
        QCOMPARE(b2.snippets().size(), 1);
        QCOMPARE(b2.appRules().size(), 1);

        // Switching to a single language clears preferred list cleanly.
        QVERIFY(b2.setLanguages("en", {}));
        QCOMPARE(b2.language(), QString("en"));
        QVERIFY(b2.preferredLanguages().isEmpty());
        QCOMPARE(b2.languagesDisplay(), QString("en"));

        b2.setOption("autostart", false);
        QVERIFY(!b2.autostart());
        QVERIFY(!QFile::exists(dir.filePath("org.sol.Dictation.desktop")));
    }

    void appStylesAndCodeContext()
    {
        QTemporaryDir dir;
        Backend b(false, nullptr, dir.path());
        b.setOption("style", "auto");
        b.setOption("personalStyle", "lowercase");
        b.setOption("workStyle", "casual");
        b.setOption("emailStyle", "formal");
        b.setOption("codeStyle", "formal");
        b.setOption("otherStyle", "excited");
        QVERIFY(b.addAppRule("obsidian", "casual"));

        QCOMPARE(b.resolveAppStyle({{"app", "thunderbird"}}), QString("formal"));
        QCOMPARE(b.resolveAppStyle({{"app", "slack"}}), QString("casual"));
        QCOMPARE(b.resolveAppStyle({{"app", "discord"}}), QString("lowercase"));
        QCOMPARE(b.resolveAppStyle({{"app", "obsidian"}}), QString("casual"));
        QCOMPARE(b.resolveAppStyle({{"app", "libreoffice"}}), QString("excited"));

        QVERIFY(b.isCodeContext({{"app", "cursor"}}));
        QVERIFY(b.isCodeContext({{"app", "ghostty"}}));
        QVERIFY(b.isCodeContext({{"app", "firefox"}, {"title", "Pull Request - GitHub"}}));
        QVERIFY(!b.isCodeContext({{"app", "thunderbird"}, {"title", "Inbox"}}));

        QVERIFY(b.isUrlBarContext({{"urlBar", true}}));
        QVERIFY(b.isUrlBarContext({{"before", "https://github.com/"}}));
        QVERIFY(b.isUrlBarContext({{"app", "walker"}}));
        QVERIFY(b.isUrlBarContext({{"app", "brave-browser"}, {"title", "New Tab - Brave"}}));
        QVERIFY(b.isUrlBarContext({{"app", "helium"}, {"title", "New Tab - Helium"}}));
        QVERIFY(!b.isUrlBarContext({{"app", "firefox"}, {"before", "Hello world"}}));
    }

    void historyLearningRevertReformatAndStats()
    {
        QTemporaryDir dir;
        const QDate today = QDate::currentDate();
        const QDate yesterday = today.addDays(-1);
        QJsonObject seed{
            {"history", QJsonArray{
                QJsonObject{
                    {"id", "h1"},
                    {"text", "Let's meet Kody tomorrow."},
                    {"raw", "um lets meet kody tomorrow"},
                    {"language", "en"},
                    {"created", QDateTime(today, QTime(10, 0)).toString(Qt::ISODateWithMs)},
                    {"duration", 2.0},
                },
                QJsonObject{
                    {"id", "h2"},
                    {"text", "Ship the release build right now."},
                    {"raw", "ship the release build right now"},
                    {"language", "en"},
                    {"created", QDateTime(yesterday, QTime(14, 0)).toString(Qt::ISODateWithMs)},
                    {"duration", 2.0},
                },
            }},
            {"stats", QJsonObject{
                {"words", 10},
                {"dictations", 2},
                {"timedWords", 10},
                {"timedSeconds", 4.0},
                {"days", QJsonArray{yesterday.toString(Qt::ISODate), today.toString(Qt::ISODate)}},
            }},
        };
        QFile f(dir.filePath("personal.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(seed).toJson(QJsonDocument::Compact));
        f.close();

        Backend b(false, nullptr, dir.path());
        QCOMPARE(b.totalWords(), 10);
        QCOMPARE(b.totalDictations(), 2);
        QCOMPARE(b.averageWpm(), 150);
        QCOMPARE(b.streakDays(), 2);

        QVERIFY(b.editHistory("h1", "Let's meet Cody tomorrow."));
        QCOMPARE(b.dictionary().size(), 1);
        const auto learned = b.dictionary().first().toMap();
        QCOMPARE(learned["trigger"].toString(), QString("Kody"));
        QCOMPARE(learned["value"].toString(), QString("Cody"));
        QCOMPARE(learned["autoLearned"].toBool(), true);

        QVERIFY(b.reformatHistory("h2", "excited"));
        QCOMPARE(b.history().at(1).toMap()["text"].toString(), QString("Ship the release build right now!"));

        QVERIFY(b.revertToRaw("h1"));
        QCOMPARE(b.history().first().toMap()["text"].toString(), QString("um lets meet kody tomorrow"));
    }

    void chordConvertersAndSingleLuaShortcut()
    {
        QCOMPARE(Desktop::toHyprChord("Super+Alt+V"), QString("SUPER ALT, V"));
        QCOMPARE(Desktop::toHyprChord("Ctrl+Alt+Space"), QString("CTRL ALT, SPACE"));
        QCOMPARE(Desktop::toHyprChord("F9"), QString(", F9"));
        QCOMPARE(Desktop::toHyprLuaChord("Super+Alt+V"), QString("SUPER + ALT + V"));
        QCOMPARE(Desktop::toHyprLuaChord("F9"), QString("F9"));
        QCOMPARE(Desktop::toHyprLuaChord("Alt_R"), QString("ALT + Alt_R"));
        const QString lua = Desktop::hyprLuaScript("F9", false);
        QVERIFY(lua.contains("sol_bind_hold(\"F9\""));
        QVERIFY(!lua.contains("sol-dictation-glass"));
        QVERIFY(!lua.contains("command-start"));
        QVERIFY(!lua.contains("paste-last"));
        const QString glassLua = Desktop::hyprLuaScript("F9", true);
        QVERIFY(glassLua.contains("sol-dictation-glass"));
        QVERIFY(glassLua.contains("blur = true"));
        QCOMPARE(Desktop::toSwayChord("Super+Alt+V"), QString("Mod4+Mod1+v"));
        QCOMPARE(Desktop::toNiriChord("Super+Alt+V"), QString("Mod+Alt+V"));
        QCOMPARE(Desktop::toGnomeChord("Super+Alt+V"), QString("<Super><Alt>v"));
        QCOMPARE(Desktop::toPortalChord("Super+Alt+V"), QString("LOGO+ALT+v"));
    }

    void audioCuesSynthesizeValidWav()
    {
        for (auto cue : {AudioFeedback::Cue::Start, AudioFeedback::Cue::Stop,
                         AudioFeedback::Cue::Lock, AudioFeedback::Cue::Done, AudioFeedback::Cue::Cancel}) {
            const QByteArray wav = AudioFeedback::buildWav(cue);
            QVERIFY(wav.size() > 2000);
            QCOMPARE(wav.left(4), QByteArray("RIFF"));
            QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
            QCOMPARE(wav.mid(36, 4), QByteArray("data"));
            const quint32 dataBytes = qFromLittleEndian<quint32>(wav.constData() + 40);
            QCOMPARE(static_cast<qsizetype>(dataBytes), wav.size() - 44);
            int peak = 0;
            for (qsizetype i = 44; i + 1 < wav.size(); i += 2)
                peak = std::max(peak, std::abs(static_cast<int>(qFromLittleEndian<qint16>(wav.constData() + i))));
            QVERIFY(peak > 500);
        }
    }

    void hudWindowRendersAllActiveStatesWithoutQml()
    {
        HudWindow hud;
        // Idle with no toast must be hidden (no idle mini bar).
        hud.applyState({{"phase", "idle"}, {"hud", true}, {"snap", true}});
        QVERIFY(!hud.shouldBeVisible());
        QVERIFY(!hud.isInteractive());

        // Verify both "mono" (pure black & white) and "glass" (translucent frosted blur) render cleanly.
        const QList<QJsonObject> states = {
            {{"phase", "starting"}, {"hud", true}, {"theme", "mono"}, {"snap", true}},
            {{"phase", "recording"}, {"mode", "ptt"}, {"hud", true}, {"theme", "mono"}, {"level", 0.72}, {"snap", true}},
            {{"phase", "recording"}, {"mode", "handsfree"}, {"hud", true}, {"theme", "mono"}, {"level", 0.8}, {"snap", true}},
            {{"phase", "processing"}, {"hud", true}, {"theme", "mono"}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "mono"}, {"toast", "Pasted"}, {"toastKind", "ok"}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "mono"}, {"editing", true}, {"editText", "arm -> ARM"}, {"snap", true}},
            {{"phase", "starting"}, {"hud", true}, {"theme", "glass"}, {"editing", false}, {"snap", true}},
            {{"phase", "recording"}, {"mode", "ptt"}, {"hud", true}, {"theme", "glass"}, {"level", 0.72}, {"snap", true}},
            {{"phase", "recording"}, {"mode", "ptt"}, {"whisper", true}, {"hud", true}, {"theme", "glass"}, {"level", 0.5}, {"snap", true}},
            {{"phase", "recording"}, {"mode", "handsfree"}, {"hud", true}, {"theme", "glass"}, {"level", 0.8}, {"snap", true}},
            {{"phase", "processing"}, {"hud", true}, {"theme", "glass"}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "glass"}, {"toast", "Pasted"}, {"toastKind", "ok"}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "glass"}, {"toast", "Password field blocked"}, {"toastKind", "error"}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "glass"}, {"editing", true}, {"editText", ""}, {"snap", true}},
            {{"phase", "idle"}, {"hud", true}, {"theme", "glass"}, {"editing", true}, {"editText", "arm -> ARM"}, {"snap", true}},
        };
        for (const auto &s : states) {
            hud.applyState(s);
            QVERIFY(hud.shouldBeVisible());
            const QImage img = hud.renderPreviewImage();
            QCOMPARE(img.size(), QSize(380, 58));
            int visiblePixels = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 60) ++visiblePixels;
            QVERIFY(visiblePixels > 2000);
        }
        // Confirm mono pill body is pure opaque black (no translucency), while glass body is translucent for blur.
        hud.applyState({{"phase", "recording"}, {"mode", "ptt"}, {"hud", true}, {"theme", "mono"}, {"editing", false}, {"level", 0.5}, {"snap", true}});
        const QImage monoImg = hud.renderPreviewImage();
        QCOMPARE(qAlpha(monoImg.pixel(155, 34)), 255);
        hud.applyState({{"phase", "recording"}, {"mode", "ptt"}, {"hud", true}, {"theme", "glass"}, {"editing", false}, {"level", 0.5}, {"snap", true}});
        const QImage glassImg = hud.renderPreviewImage();
        const int glassAlpha = qAlpha(glassImg.pixel(155, 34));
        QVERIFY2(glassAlpha >= 60 && glassAlpha < 200, "Glass pill body must stay translucent for compositor blur");

        // Verify typed rule parsing ("arm -> ARM", "slash = /", "Slash = /", "-arm", standalone word).
        auto r1 = HudWindow::parseRule("arm -> ARM");
        QCOMPARE(r1.action, HudRule::Action::Learn);
        QCOMPARE(r1.trigger, QString("arm"));
        QCOMPARE(r1.value, QString("ARM"));

        auto r2 = HudWindow::parseRule("slash = /");
        QCOMPARE(r2.action, HudRule::Action::Learn);
        QCOMPARE(r2.trigger, QString("slash"));
        QCOMPARE(r2.value, QString("/"));

        auto r3 = HudWindow::parseRule("my url -> https://example.com");
        QCOMPARE(r3.action, HudRule::Action::Learn);
        QCOMPARE(r3.trigger, QString("my url"));
        QCOMPARE(r3.value, QString("https://example.com"));

        auto r4 = HudWindow::parseRule("-arm");
        QCOMPARE(r4.action, HudRule::Action::Forget);
        QCOMPARE(r4.trigger, QString("arm"));

        auto r5 = HudWindow::parseRule("Hyprland");
        QCOMPARE(r5.action, HudRule::Action::Learn);
        QCOMPARE(r5.trigger, QString("Hyprland"));
        QCOMPARE(r5.value, QString("Hyprland"));

        // Disabled HUD stays hidden even while recording.
        hud.applyState({{"phase", "recording"}, {"editing", false}, {"hud", false}, {"snap", true}});
        QVERIFY(!hud.shouldBeVisible());
    }

    void cliStyleHasNoEmojisOrFluffAndRespectsColor()
    {
        qputenv("FORCE_COLOR", "1");
        qunsetenv("NO_COLOR");
        QVERIFY(CliUi::good("OK").contains("\033[1;32m"));
        QVERIFY(CliUi::bad("FAIL").contains("\033[1;31m"));
        QVERIFY(CliUi::warn("WARN").contains("\033[1;33m"));
        QVERIFY(CliUi::imp("HOT").contains("\033["));

        qputenv("NO_COLOR", "1");
        QCOMPARE(CliUi::good("OK"), QString("OK"));
        QCOMPARE(CliUi::bad("FAIL"), QString("FAIL"));

        QString helpText;
        QTextStream out(&helpText);
        CliUi::printHelp(out);
        QVERIFY(!helpText.contains("Wispr", Qt::CaseInsensitive));
        QVERIFY(!helpText.contains("polish", Qt::CaseInsensitive));
        QVERIFY(!helpText.contains(QChar(0x2600))); // no sun emoji
        for (const QChar ch : helpText)
            QVERIFY2(ch.unicode() < 128, "CLI help output must stay clean ASCII with no emojis");

        qunsetenv("NO_COLOR");
        qunsetenv("FORCE_COLOR");
    }
};

QTEST_MAIN(BackendTests)
#include "backend_tests.moc"
