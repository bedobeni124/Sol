#include "../src/formatter.h"
#include <QDate>
#include <QTest>

namespace {
QVariant entry(const QString &trigger, const QString &value)
{
    return QVariantMap{{"trigger", trigger}, {"value", value}};
}
} // namespace

class FormatterTests : public QObject {
    Q_OBJECT
private slots:
    void dictation_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("expected");
        QTest::newRow("hesitations") << "Um, I, I think the, uh, bridge is ready." << "I think the bridge is ready.";
        QTest::newRow("phrase stutter") << "I think, I think we should ship today." << "I think we should ship today.";
        QTest::newRow("date") << "send it Monday, actually Tuesday." << "Send it Tuesday.";
        QTest::newRow("time") << "Let's meet at 11 actually 12." << "Let's meet at 12.";
        QTest::newRow("proper noun swap") << "Send the draft to Sarah, actually Marcus." << "Send the draft to Marcus.";
        QTest::newRow("make that") << "Let's schedule lunch, make that dinner." << "Let's schedule dinner.";
        QTest::newRow("unchanged") << "I actually liked the movie." << "I actually liked the movie.";
        QTest::newRow("cancellation") << "Keep this. Use the long introduction. Scratch that, skip the intro."
                                      << "Keep this. Skip the intro.";
        QTest::newRow("delete last sentence") << "Keep this. Drop that part. Delete last sentence."
                                              << "Keep this.";
        QTest::newRow("restart") << "we should drive to Athens, actually we should fly to Oslo."
                                 << "We should fly to Oslo.";
        QTest::newRow("exact punctuation") << "hello, comma, Jamie, question mark?" << "Hello, Jamie?";
        QTest::newRow("new paragraph") << "Good morning. New paragraph. are we ready question mark"
                                       << "Good morning.\n\nAre we ready?";
        QTest::newRow("literal punctuation") << "remove the comma after this short period of time."
                                             << "Remove the comma after this short period of time.";
        QTest::newRow("list") << "The steps: first, open the case, second, turn the latch, third, close the door."
                              << "The steps:\n1. Open the case\n2. Turn the latch\n3. Close the door.";
        QTest::newRow("cardinal list") << "One, open the file. Two, send a copy." << "1. Open the file.\n2. Send a copy.";
        QTest::newRow("not a list") << "I bought one pear and two apples." << "I bought one pear and two apples.";
        QTest::newRow("bullet") << "bullet point buy apples next bullet buy pears" << "- Buy apples\n- Buy pears.";
        QTest::newRow("spoken time") << "let's sync at 3 30 pm" << "Let's sync at 3:30 PM.";
        QTest::newRow("spoken email") << "email alice at example dot com" << "Email alice@example.com.";
        QTest::newRow("spoken url") << "check github dot com slash spn slash sol" << "Check github.com/spn/sol.";
        QTest::newRow("inline uppercase suffix") << "build for arm in uppercase Linux" << "Build for ARM Linux.";
        QTest::newRow("inline uppercase single") << "any in uppercase" << "ANY.";
        QTest::newRow("inline uppercase prefix") << "mark this all caps urgent" << "Mark this URGENT.";
        QTest::newRow("inline uppercase literal prose") << "write it in uppercase letters" << "Write it in uppercase letters.";
        QTest::newRow("inline lowercase suffix") << "keep API in lowercase" << "Keep api.";
        QTest::newRow("all filler") << "Um." << "";
    }
    void dictation()
    {
        QFETCH(QString, raw);
        QFETCH(QString, expected);
        QCOMPARE(Formatter::format(raw, "en-US", "light", "formal", {}, {}), expected);
    }
    void plain()
    {
        QCOMPARE(Formatter::format("um, send it Monday actually Tuesday", "en", "none", "formal", {}, {}),
                 "um, send it Monday actually Tuesday");
        QCOMPARE(Formatter::format("a note new line", "en", "none", "casual", {}, {}), "a note\n");
    }
    void personalized()
    {
        const QVariantList words{entry("blue jay", "BlueJay"), entry("jay", "JAY"), entry("don", "Dawn")};
        const QVariantList snippets{entry("my details", "blue jay\n\n<Alias>  10:00am\n"), entry("details", "short")};
        QCOMPARE(Formatter::format("My, details!", "en", "light", "lowercase", words, snippets), "blue jay\n\n<Alias>  10:00am\n");
        QCOMPARE(Formatter::format("BLUE JAY's my details!", "en", "light", "lowercase", words, snippets),
                 "BlueJay's blue jay\n\n<Alias>  10:00am\n!");
        QCOMPARE(Formatter::format("don’t call don or jayhawk.", "en", "light", "formal", words, snippets),
                 "Don’t call Dawn or jayhawk.");
    }
    void dynamicSnippets()
    {
        FormatOptions opts;
        opts.language = "en";
        opts.cleanup = "light";
        opts.style = "formal";
        opts.clipboardText = "https://sol.dev";
        opts.snippets = {entry("daily note", "Log {{iso_date}}: {{clipboard}}")};
        const QString expected = QStringLiteral("Log %1: https://sol.dev").arg(QDate::currentDate().toString("yyyy-MM-dd"));
        QCOMPARE(Formatter::format("daily note", opts), expected);
    }
    void codeMode()
    {
        FormatOptions opts;
        opts.language = "en";
        opts.cleanup = "light";
        opts.style = "formal";
        opts.codeMode = true;
        opts.fileTagging = true;
        opts.contextSymbols = {"isLoginError", "SpeechEngine", "main.cpp", "user_profile_id"};

        QCOMPARE(Formatter::format("rename camel case get user name to snake case fetch user id", opts),
                 "Rename getUserName to fetch_user_id.");
        QCOMPARE(Formatter::format("create pascal case nav bar and constant case max retries", opts),
                 "Create NavBar and MAX_RETRIES.");
        QCOMPARE(Formatter::format("check kebab case flow bar pill", opts),
                 "Check flow-bar-pill.");
        QCOMPARE(Formatter::format("fix the bug in at main dot cpp where is login error is null", opts),
                 "Fix the bug in @main.cpp where isLoginError is null.");
        QCOMPARE(Formatter::format("inspect speech engine in formatter dot cpp", opts),
                 "Inspect SpeechEngine in formatter.cpp.");

        const QStringList syms = Formatter::extractSymbols("int fooBar = check_state(hudwindow.cpp, authCheck.ts);");
        QVERIFY(syms.contains("hudwindow.cpp"));
        QVERIFY(syms.contains("authCheck.ts"));
        QVERIFY(syms.contains("fooBar"));
        QVERIFY(syms.contains("check_state"));
    }
    void otherLanguage()
    {
        QCOMPARE(Formatter::format("um acht in Köln", "de", "light", "casual", {}, {}), "um acht in Köln");
        QCOMPARE(Formatter::format("Hallottam hogy réka jön.", "hu", "light", "formal", {entry("Réka", "Réka")}, {}),
                 "Hallottam hogy Réka jön.");
        QCOMPARE(Formatter::format("Első sor új sor Második sor", "hu", "light", "formal", {}, {}),
                 "Első sor\nMásodik sor");
    }
    void caseInsensitiveSymbolRules()
    {
        // "slash = /" must match "slash", "Slash", and "SLASH" identically.
        const QVariantList dict{entry("slash", "/"), entry("arm", "ARM")};
        QCOMPARE(Formatter::format("Slash", "en", "light", "formal", dict, {}), QString("/"));
        QCOMPARE(Formatter::format("slash", "en", "light", "formal", dict, {}), QString("/"));
        QCOMPARE(Formatter::format("SLASH", "en", "light", "formal", dict, {}), QString("/"));
        QCOMPARE(Formatter::format("Slash help", "en", "light", "formal", dict, {}), QString("/ help."));
        QCOMPARE(Formatter::format("Build for Arm Linux", "en", "light", "formal", dict, {}), QString("Build for ARM Linux."));
    }
    void urlBarNoTrailingPeriod()
    {
        FormatOptions urlOpts;
        urlOpts.language = "en";
        urlOpts.cleanup = "light";
        urlOpts.style = "formal";
        urlOpts.noTrailingPeriod = true;

        QCOMPARE(Formatter::format("weather in Budapest.", urlOpts), QString("Weather in Budapest"));
        QCOMPARE(Formatter::format("youtube", urlOpts), QString("Youtube"));
        QCOMPARE(Formatter::format("github dot com slash spn slash sol", urlOpts), QString("github.com/spn/sol"));

        // Non-English inside URL bar also drops trailing period.
        urlOpts.language = "hu";
        QCOMPARE(Formatter::format("időjárás Budapest.", urlOpts), QString("időjárás Budapest"));

        // Standalone URLs (including 1-char domains like x.com) drop trailing period and lowercase domain.
        QCOMPARE(Formatter::format("x.com", "en", "light", "formal", {}, {}), QString("x.com"));
        QCOMPARE(Formatter::format("X.com.", "en", "light", "formal", {}, {}), QString("x.com"));
        QCOMPARE(Formatter::format("x dot com", "en", "light", "formal", {}, {}), QString("x.com"));
        QCOMPARE(Formatter::format("instagram.com", "en", "light", "formal", {}, {}), QString("instagram.com"));
        QCOMPARE(Formatter::format("github dot com", "en", "light", "formal", {}, {}), QString("github.com"));
        QCOMPARE(Formatter::format("GitHub.com/Benedek/Sol.", "en", "light", "formal", {}, {}), QString("github.com/Benedek/Sol"));
        // Ellipsis is preserved.
        urlOpts.language = "en";
        QCOMPARE(Formatter::format("wait...", urlOpts), QString("Wait..."));
    }
    void tones()
    {
        QCOMPARE(Formatter::format("see you tomorrow.", "en", "light", "casual", {}, {}), "See you tomorrow");
        QCOMPARE(Formatter::format("Great work.", "en", "light", "excited", {}, {}), "Great work!");
        QCOMPARE(Formatter::format("I want to help in order to save time.", "en", "medium", "formal", {}, {}),
                 "I want to help to save time.");
    }
};

QTEST_GUILESS_MAIN(FormatterTests)
#include "formatter_tests.moc"
