#include "formatter.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace {
using Regex = QRegularExpression;

Regex expression(const QString &pattern)
{
    return Regex(pattern, Regex::CaseInsensitiveOption | Regex::UseUnicodePropertiesOption);
}

QString key(const QString &value)
{
    return value.simplified().toCaseFolded();
}

QString looseKey(QString value)
{
    static const Regex separators(R"([\p{P}\p{S}\s]+)");
    return value.normalized(QString::NormalizationForm_C).remove(separators).toCaseFolded();
}

QString expandSnippetVariables(QString value, const QString &clipboardText)
{
    if (!value.contains("{{")) return value;
    const QDateTime now = QDateTime::currentDateTime();
    value.replace("{{iso_date}}", now.toString("yyyy-MM-dd"), Qt::CaseInsensitive);
    value.replace("{{date}}", QLocale().toString(now.date(), QLocale::ShortFormat), Qt::CaseInsensitive);
    value.replace("{{long_date}}", QLocale().toString(now.date(), QLocale::LongFormat), Qt::CaseInsensitive);
    value.replace("{{time}}", QLocale().toString(now.time(), QLocale::ShortFormat), Qt::CaseInsensitive);
    value.replace("{{datetime}}", QLocale().toString(now, QLocale::ShortFormat), Qt::CaseInsensitive);
    value.replace("{{clipboard}}", clipboardText, Qt::CaseInsensitive);
    return value;
}

struct Rules {
    QHash<QString, QString> values;
    QHash<QString, QString> standalone;
    Regex pattern;

    Rules(const QVariantList &dictionary, const QVariantList &snippets, const QString &clipboardText = {})
    {
        auto add = [this, &clipboardText](const QVariant &item, bool snippet) {
            const QVariantMap map = item.toMap();
            const QString trigger = map.value("trigger").toString().simplified();
            if (trigger.isEmpty())
                return;
            QString value = map.value("value").toString();
            if (!snippet && value.isEmpty())
                value = trigger;
            if (snippet)
                value = expandSnippetVariables(value, clipboardText);
            values.insert(key(trigger), value);
            const QString loose = looseKey(trigger);
            if (snippet && !loose.isEmpty())
                standalone.insert(loose, value);
        };
        for (const QVariant &entry : dictionary) add(entry, false);
        for (const QVariant &entry : snippets) add(entry, true);
        QStringList words = values.keys();
        std::sort(words.begin(), words.end(), [](const QString &a, const QString &b) {
            return a.size() > b.size();
        });
        QStringList branches;
        for (const QString &word : words) {
            QStringList parts = word.split(' ');
            for (QString &part : parts) part = Regex::escape(part);
            QString branch = parts.join(R"(\h+)");
            // A name can be possessive, but "don" must never match the start of "don't".
            if (!word.contains(u'’') && !word.contains('\''))
                branch += R"((?!['’ʼ][tT]\b))";
            branches.append(branch);
        }
        if (!branches.isEmpty())
            pattern = expression(R"((?<![\p{L}\p{M}\p{N}_])(?:)" + branches.join('|')
                                 + R"()(?![\p{L}\p{M}\p{N}_]))");
    }
};

struct Literal {
    QString marker;
    QString value;
};

QString nextDelimiter(const QString &text)
{
    QString delimiter(QChar(0xe001));
    while (text.contains(delimiter)) delimiter.append(QChar(0xe001));
    return delimiter;
}

QString maskEntries(const QString &text, const Rules &rules, QList<Literal> &literals, const QString &delimiter)
{
    if (rules.values.isEmpty()) return text;
    QString out;
    qsizetype start = 0;
    for (const auto &m : rules.pattern.globalMatch(text)) {
        const QString token = delimiter + QString::number(literals.size()) + delimiter;
        literals.append({token, rules.values.value(key(m.captured()))});
        out += text.sliced(start, m.capturedStart() - start) + token;
        start = m.capturedEnd();
    }
    return out + text.sliced(start);
}

// Perform edits from right to left. Captured positions always refer to the unedited input.
template<typename F>
QString substitute(QString text, const Regex &pattern, F replacement)
{
    const QString original = text;
    QList<QRegularExpressionMatch> matches;
    for (const auto &m : pattern.globalMatch(original)) matches.append(m);
    for (auto it = matches.crbegin(); it != matches.crend(); ++it)
        text.replace(it->capturedStart(), it->capturedLength(), replacement(*it, original));
    return text;
}

QString spokenCodeCasing(QString text, QList<Literal> &literals, const QString &delimiter)
{
    // Spoken identifier casing commands: "camel case get user id" -> getUserId
    static const Regex casing = expression(
        R"(\b(camel|pascal|snake|kebab|constant|screaming snake)\h+case\h+([\p{L}\p{N}]+(?:\h+[\p{L}\p{N}]+){0,5}))");
    static const QSet<QString> stopWords = {
        "and", "or", "is", "are", "was", "were", "to", "in", "on", "at", "for", "with",
        "from", "of", "by", "as", "if", "then", "that", "which", "when", "where", "should",
        "will", "can", "could", "would", "has", "have", "had", "returns", "return", "equals",
        "takes", "accepts", "instead", "actually", "please", "because", "so", "but",
        "camel", "pascal", "snake", "kebab", "constant", "screaming"
    };
    // Loop because a single sentence can contain multiple casing commands ("rename camel case X to snake case Y").
    for (int pass = 0; pass < 8; ++pass) {
        const auto m = casing.match(text);
        if (!m.hasMatch()) break;
        const QString mode = m.captured(1).toLower();
        const QStringList rawWords = m.captured(2).split(Regex(R"(\h+)"), Qt::SkipEmptyParts);
        QStringList identWords;
        QStringList trailing;
        for (int i = 0; i < rawWords.size(); ++i) {
            if (!trailing.isEmpty() || (i >= 1 && stopWords.contains(rawWords[i].toLower()))) {
                trailing.append(rawWords[i]);
            } else {
                identWords.append(rawWords[i].toLower());
            }
        }
        if (identWords.isEmpty()) break;
        QString built;
        if (mode == "camel") {
            built = identWords[0];
            for (int i = 1; i < identWords.size(); ++i)
                built += identWords[i].left(1).toUpper() + identWords[i].mid(1);
        } else if (mode == "pascal") {
            for (const QString &w : identWords)
                built += w.left(1).toUpper() + w.mid(1);
        } else if (mode == "snake") {
            built = identWords.join('_');
        } else if (mode == "kebab") {
            built = identWords.join('-');
        } else {
            built = identWords.join('_').toUpper();
        }
        const QString token = delimiter + QString::number(literals.size()) + delimiter;
        literals.append({token, built});
        const QString repl = trailing.isEmpty() ? token : token + " " + trailing.join(' ');
        text.replace(m.capturedStart(), m.capturedLength(), repl);
    }
    return text;
}

QString spokenInlineCasing(QString text, QList<Literal> &literals, const QString &delimiter)
{
    auto stash = [&](const QString &literal) {
        const QString token = delimiter + QString::number(literals.size()) + delimiter;
        literals.append({token, literal});
        return token;
    };
    static const QSet<QString> nonTargetNext = {
        "letter", "letters", "character", "characters", "text", "format", "mode", "only",
        "and", "or", "is", "are", "was", "were", "to", "for", "of", "with", "in", "on", "at", "by", "as", "into",
    };
    static const QSet<QString> nonTargetPrev = {
        "it", "this", "that", "these", "those", "them", "everything", "something", "nothing", "anything",
        "written", "write", "type", "put", "set", "make", "format", "keep", "be", "is", "are",
        "say", "said", "speak", "spoke", "talk", "paste", "pasted",
    };

    // Suffix form: "any in uppercase", "arm in all caps", "urgent, all caps", "any, uppercase"
    static const Regex suffixUpper = expression(
        R"((?:\b(?:the\h+)?word\h+)?\b([\p{L}\p{N}][\p{L}\p{N}'’_-]*)(?:(?:,\h*|\h+)(?:in\h+upper\h*case|in\h+all\h+caps|all\h+caps)|,\h*upper\h*case)\b(?!\h+(?:letters?|characters?|text|format|mode)\b))");
    text = substitute(text, suffixUpper, [&](const auto &m, const QString &) {
        const QString w = m.captured(1);
        if (nonTargetPrev.contains(w.toLower()) && !m.captured(0).contains("word", Qt::CaseInsensitive))
            return m.captured(0);
        return stash(w.toUpper());
    });

    static const Regex suffixLower = expression(
        R"((?:\b(?:the\h+)?word\h+)?\b([\p{L}\p{N}][\p{L}\p{N}'’_-]*)(?:(?:,\h*|\h+)in\h+lower\h*case|,\h*lower\h*case)\b(?!\h+(?:letters?|characters?|text|format|mode)\b))");
    text = substitute(text, suffixLower, [&](const auto &m, const QString &) {
        const QString w = m.captured(1);
        if (nonTargetPrev.contains(w.toLower()) && !m.captured(0).contains("word", Qt::CaseInsensitive))
            return m.captured(0);
        return stash(w.toLower());
    });

    // Prefix form: "uppercase any", "all caps urgent", "in uppercase arm", "lowercase API"
    static const Regex prefixUpper = expression(
        R"(\b(?:in\h+)?(?:upper\h*case|all\h+caps)(?:,\h*|\h+)([\p{L}\p{N}][\p{L}\p{N}'’_-]*)\b)");
    text = substitute(text, prefixUpper, [&](const auto &m, const QString &) {
        const QString w = m.captured(1);
        if (nonTargetNext.contains(w.toLower())) return m.captured(0);
        return stash(w.toUpper());
    });

    static const Regex prefixLower = expression(
        R"(\b(?:in\h+)?lower\h*case(?:,\h*|\h+)([\p{L}\p{N}][\p{L}\p{N}'’_-]*)\b)");
    text = substitute(text, prefixLower, [&](const auto &m, const QString &) {
        const QString w = m.captured(1);
        if (nonTargetNext.contains(w.toLower())) return m.captured(0);
        return stash(w.toLower());
    });

    return text;
}

QString spokenCodeAndWeb(QString text, bool codeMode, bool fileTagging,
                         const QStringList &contextSymbols, QList<Literal> &literals,
                         const QString &delimiter)
{
    auto stash = [&](const QString &literal) {
        const QString token = delimiter + QString::number(literals.size()) + delimiter;
        literals.append({token, literal});
        return token;
    };

    // Inline spoken uppercase / lowercase works in every app, not only code editors.
    text = spokenInlineCasing(text, literals, delimiter);

    // Spoken email addresses: "alice at example dot com" -> alice@example.com
    static const Regex email = expression(
        R"(\b([a-z0-9][a-z0-9._+-]{0,40})\h+at\h+([a-z0-9][a-z0-9-]{0,40})\h+dot\h+(com|org|net|io|ai|dev|co|edu|gov|app|me|uk|de|fr|hu|eu)\b)");
    text = substitute(text, email, [&](const auto &m, const QString &) {
        return stash((m.captured(1) + "@" + m.captured(2) + "." + m.captured(3)).toLower());
    });

    // Spoken URLs: "github dot com slash foo slash bar" -> github.com/foo/bar, "x dot com" -> x.com
    static const Regex url = expression(
        R"(\b([a-z0-9](?:[a-z0-9-]{0,40})?)\h+dot\h+(com|org|net|io|ai|dev|app|co|gg|tv|me|rs|sh|uk|de|fr|hu|eu)(?:\h+slash\h+([a-z0-9._-]+(?:\h+slash\h+[a-z0-9._-]+)*))?\b)");
    text = substitute(text, url, [&](const auto &m, const QString &) {
        QString out = (m.captured(1) + "." + m.captured(2)).toLower();
        if (!m.captured(3).isEmpty()) {
            QString path = m.captured(3);
            path.replace(expression(R"(\h+slash\h+)"), "/");
            out += "/" + path;
        }
        return stash(out);
    });

    if (!codeMode) return text;

    text = spokenCodeCasing(text, literals, delimiter);

    // Spoken filenames with common developer extensions: "main dot cpp" -> main.cpp
    // Optional "at" / "tag" prefix when fileTagging is active: "at main dot cpp" -> @main.cpp
    static const Regex spokenFile = expression(
        R"((?:\b(at|tag)\h+)?\b([a-z0-9][a-z0-9_-]{0,40})\h+dot\h+(cpp|cc|cxx|hpp|h|c|py|qml|js|jsx|ts|tsx|rs|go|java|kt|swift|rb|php|cs|sh|bash|zsh|json|yaml|yml|toml|xml|html|css|scss|sql|md|txt|pro|cmake|lua|nix|env|lock|svg|png|jpg|csv|qrc)\b)");
    QHash<QString, QString> contextFilesByLower;
    for (const QString &sym : contextSymbols) {
        if (sym.contains('.')) contextFilesByLower.insert(sym.toLower(), sym);
    }
    text = substitute(text, spokenFile, [&](const auto &m, const QString &) {
        const bool tagged = fileTagging && !m.captured(1).isEmpty();
        const QString rawName = m.captured(2) + "." + m.captured(3).toLower();
        const QString resolved = contextFilesByLower.value(rawName.toLower(), rawName);
        return stash((tagged ? "@" : "") + resolved);
    });

    // Match spoken multi-word references to visible camelCase / PascalCase / snake_case context symbols.
    // E.g. if "isLoginError" is in contextSymbols and user says "is login error", restore "isLoginError".
    if (!contextSymbols.isEmpty()) {
        struct SymMatch {
            QString spoken;
            QString exact;
            bool isFile = false;
        };
        QList<SymMatch> symRules;
        QSet<QString> seen;
        static const Regex camelSplit(R"((?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])|[_-]+)");
        for (const QString &sym : contextSymbols) {
            if (sym.size() < 4) continue;
            const bool isFile = sym.contains('.');
            QString base = isFile ? sym.section('.', 0, -2) : sym;
            QStringList parts = base.split(camelSplit, Qt::SkipEmptyParts);
            // Only rewrite multi-part identifiers (camelCase/snake_case/filenames) so normal English words stay untouched.
            if (parts.size() < 2 && !isFile) continue;
            if (parts.size() >= 2) {
                const QString spoken = parts.join(' ').toLower();
                if (spoken.size() >= 5 && !seen.contains(spoken)) {
                    seen.insert(spoken);
                    symRules.append({spoken, sym, isFile});
                }
            }
            if (isFile) {
                const QString lower = sym.toLower();
                if (!seen.contains(lower)) {
                    seen.insert(lower);
                    symRules.append({lower, sym, true});
                }
            }
        }
        std::sort(symRules.begin(), symRules.end(), [](const SymMatch &a, const SymMatch &b) {
            return a.spoken.size() > b.spoken.size();
        });
        for (const SymMatch &rule : symRules) {
            QStringList parts = rule.spoken.split(' ', Qt::SkipEmptyParts);
            for (QString &p : parts) p = Regex::escape(p);
            const QString prefix = (fileTagging && rule.isFile) ? R"rx((?:\b(?:at|tag)\h+)?)rx" : "";
            const Regex pat = expression(prefix + R"rx((?<![\p{L}\p{N}_]))rx" + parts.join(R"(\h+)") + R"rx((?![\p{L}\p{N}_]))rx");
            text = substitute(text, pat, [&](const auto &m, const QString &) {
                const bool tagged = fileTagging && rule.isFile
                    && (m.captured().startsWith("at ", Qt::CaseInsensitive)
                        || m.captured().startsWith("tag ", Qt::CaseInsensitive));
                return stash((tagged ? "@" : "") + rule.exact);
            });
        }
    }

    // Spoken code symbols (only in codeMode)
    static const QList<QPair<Regex, QString>> codeSymbols = {
        {expression(R"(\h*\bopen paren(?:thesis)?\b\h*)"), "("},
        {expression(R"(\h*\bclose paren(?:thesis)?\b)"), ")"},
        {expression(R"(\h*\bopen bracket\b\h*)"), "["},
        {expression(R"(\h*\bclose bracket\b)"), "]"},
        {expression(R"(\h*\bopen brace\b\h*)"), "{"},
        {expression(R"(\h*\bclose brace\b)"), "}"},
        {expression(R"(\bdouble equals\b)"), "=="},
        {expression(R"(\bnot equals\b)"), "!="},
        {expression(R"(\bfat arrow\b)"), "=>"},
        {expression(R"(\bbacktick\b)"), "`"},
    };
    for (const auto &pair : codeSymbols) {
        text.replace(pair.first, pair.second);
    }
    return text;
}

QString commands(QString text, bool breaksOnly, bool english)
{
    static const QHash<QString, QString> marks = {
        {"start a new paragraph", "\n\n"}, {"new paragraph", "\n\n"},
        {"új bekezdés", "\n\n"}, {"neuer absatz", "\n\n"}, {"nouveau paragraphe", "\n\n"}, {"nuevo párrafo", "\n\n"},
        {"new line", "\n"}, {"next line", "\n"}, {"skip a line", "\n"}, {"line break", "\n"},
        {"új sor", "\n"}, {"neue zeile", "\n"}, {"nouvelle ligne", "\n"}, {"nueva línea", "\n"},
        {"question mark", "?"}, {"exclamation mark", "!"}, {"exclamation point", "!"},
        {"full stop", "."}, {"period", "."}, {"comma", ","}, {"colon", ":"},
        {"semicolon", ";"}, {"semi colon", ";"}, {"em dash", "—"}, {"em-dash", "—"},
        {"emdash", "—"}, {"en dash", "–"}, {"en-dash", "–"}, {"ellipsis", "…"}, {"dot dot dot", "..."},
        {"bullet point", "\n- "}, {"next bullet", "\n- "}, {"new bullet", "\n- "},
        {"thumbs up emoji", "👍"}, {"fire emoji", "🔥"}, {"heart emoji", "❤️"},
        {"rocket emoji", "🚀"}, {"check mark emoji", "✅"}, {"sparkles emoji", "✨"},
        {"laughing emoji", "😂"}, {"smile emoji", "😊"}, {"smiley face", "😊"},
        {"thinking emoji", "🤔"}, {"eyes emoji", "👀"}, {"clap emoji", "👏"},
        {"party emoji", "🎉"}, {"100 emoji", "💯"}, {"warning emoji", "⚠️"},
        {"wave emoji", "👋"}, {"pray emoji", "🙏"}, {"tada emoji", "🎉"},
    };
    static const Regex command = expression(
        R"(\h*([,.;:!?]?)\h*\b(start a new paragraph|new paragraph|új bekezdés|neuer absatz|nouveau paragraphe|nuevo párrafo|skip a line|new line|next line|line break|új sor|neue zeile|nouvelle ligne|nueva línea|question mark|exclamation (?:point|mark)|full stop|semi ?colon|period|comma|colon|em[ -]?dash|en[ -]?dash|ellipsis|dot dot dot|bullet point|next bullet|new bullet|(?:thumbs up|fire|heart|rocket|check mark|sparkles|laughing|smile|thinking|eyes|clap|party|100|warning|wave|pray|tada) emoji|smiley face)\b([.,;:!?]?)\h*)");
    static const Regex mention = expression(
        R"(\b(?:the|a|an|this|that|each|every|another|training|grace|billing|trial|time|pay|probation|gestation|cooling.off|incubation|menstrual|fiscal|first|second|last|long|short|extended|same|literal|say|type|word|called|remove|spelled|spelling|named|between|after|before)\h*$)");
    static const Regex periodUse = expression(
        R"(^(?:of|during|between|lasts|ended|ending|starts|starting|when|was|is|in|has|had)\b)");

    return substitute(text, command, [&](const auto &m, const QString &source) {
        const QString label = m.captured(2).toLower();
        const QString symbol = marks.value(label);
        const bool line = symbol.startsWith('\n');
        const bool isBreak = symbol == "\n" || symbol == "\n\n";
        if (breaksOnly && !isBreak) return m.captured();
        if (!english && !isBreak && !label.endsWith("emoji")) return m.captured();
        const QString before = source.left(m.capturedStart()).right(64);
        const QString after = source.sliced(m.capturedEnd());
        if (mention.match(before).hasMatch() || (label == "period" && periodUse.match(after).hasMatch())
            || (label == "new line" && after.startsWith("of ", Qt::CaseInsensitive)))
            return m.captured();
        if (line)
            return (QString(".!?").contains(m.captured(1)) ? m.captured(1) : QString()) + symbol;
        if (label.endsWith("emoji") || label == "smiley face")
            return (m.capturedStart() > 0 ? QStringLiteral(" ") : QString()) + symbol
                   + (after.isEmpty() ? QString() : QStringLiteral(" "));
        return symbol + (after.isEmpty() ? QString() : QStringLiteral(" "));
    });
}

QString backtrack(QString text)
{
    // Unambiguous semantic classes where replacing the immediately preceding item is always safe.
    static const QStringList classes = {
        R"(Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday|today|tomorrow|yesterday)",
        R"(January|February|March|April|May|June|July|August|September|October|November|December)",
        R"(\$?\d+(?:[.:,]\d+)?%?(?:\h*(?:a\.?m\.?|p\.?m\.?))?|noon|midnight|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve)",
        R"(red|orange|yellow|green|blue|purple|pink|black|white|brown|grey|gray)",
    };
    static const QList<Regex> swaps = [] {
        QList<Regex> result;
        for (const auto &items : classes)
            result.append(expression(
                R"(\b(?:)" + items + R"()\b[,.;—\-\h]+(?:no[,\h]+(?:wait[,\h]+)?|wait[,\h]+no[,\h]+|actually[,\h]+|sorry[,\h]+|I mean[,\h]+|or rather[,\h]+|make that[,\h]+|scratch that[,\h]+)(?:make (?:it|that)\h+)?()"
                + items + R"()\b)"));
        return result;
    }();
    for (const auto &swap : swaps) text.replace(swap, "\\1");

    // Proper-noun / capitalized name swap: "send it to Sarah, actually Marcus" -> "send it to Marcus".
    static const Regex properSwap(
        R"(\b[A-Z][\p{L}'’]+\b[,.;—\-\h]+(?:no[,\h]+wait|wait[,\h]+no|actually|sorry|I mean|make that|or rather)[,.;—\-\h]+(?:make (?:it|that)\h+)?([A-Z][\p{L}'’]+)\b)",
        Regex::UseUnicodePropertiesOption);
    text.replace(properSwap, "\\1");

    // Explicit "make that X" / "no wait, X" after a single content word at clause boundary.
    static const Regex explicitMakeThat = expression(
        R"(\b[\p{L}\p{N}'’-]{2,}\b[,.;—\-\h]+(?:no[,\h]+wait[,\h]+)?make (?:it|that)\h+([\p{L}\p{N}'’-]{2,}(?:\h+[\p{L}\p{N}'’-]{2,})?)(?=[,.;!?\n]|$))");
    text.replace(explicitMakeThat, "\\1");

    // Clause restart sharing a 2-word opener: "we should drive to Athens, actually we should fly to Oslo."
    static const Regex restart = expression(
        R"((^|[.!?]\s+|\n)([\p{L}’']+\h+[\p{L}’']+)\b[^.!?\n]{0,220}?[,—.\h]+(?:actually|no wait|wait no|sorry|I mean|scratch that)[,.:\h]+(?=\2\b))");
    text.replace(restart, "\\1");

    // Sentence/clause cancellation: "scratch that", "never mind", "forget that", "delete that", "disregard that".
    static const Regex cancel = expression(
        R"((^|[.!?]\s+|\n)[^.!?\n]+?[.,;—\h]+(?:actually[,\h]+)?(?:scratch that|never ?mind|forget that|delete that|disregard that)\b(?=[,.;!?\n]|$)[,.;!?\h]*)");
    text.replace(cancel, "\\1");

    // "delete last sentence" / "scratch last sentence"
    static const Regex delLastSentence = expression(
        R"((^|[.!?]\s+|\n)[^.!?\n]+[.!?]\h*(?:delete|scratch|remove)\h+(?:the\h+)?last\h+sentence[.,!?\h]*)");
    text.replace(delLastSentence, "\\1");

    return text;
}

QString cleanupEnglish(QString text, bool medium)
{
    static const Regex filler = expression(
        R"((?:,\h*)?\b(?:um+|umm+|uh+|uhm+|erm+|er+m+|hm+m+|uh-huh)\b(?:[,\h]+|(?=\b)))");
    text.replace(filler, " ");

    // Multi-word phrase stutters: "I think, I think" -> "I think", "we need to, we need to" -> "we need to".
    static const Regex phraseStutter = expression(
        R"(\b([\p{L}’']+\h+[\p{L}’']+(?:\h+[\p{L}’']+)?)(?:[,\h]+\1)+\b)");
    text.replace(phraseStutter, "\\1");

    // Single-word stutters for common function words.
    static const Regex stutter = expression(
        R"(\b(I|we|the|to|a|an|my|our|can|you|is|are|was|she|he|it|they|this|that|in|on|for|and|but|so)(?:[,\h]+\1)+\b)");
    text.replace(stutter, "\\1");

    text = backtrack(text);

    if (medium) {
        static const Regex discourseStart = expression(R"((^|[.!?]\s+|\n)(?:so,?\h+|well,?\h+|basically,?\h+|honestly,?\h+|like,?\h+)(?=\p{L}))");
        static const Regex parenthetical = expression(R"(,\h*(?:you know|I mean|basically|literally)\h*,)");
        static const Regex inOrderTo = expression(R"(\bin order to\b)");
        static const Regex atThisPoint = expression(R"(\bat this point in time\b)");
        static const Regex dueTo = expression(R"(\bdue to the fact that\b)");
        static const Regex forPurpose = expression(R"(\bfor the purpose of\b)");
        text.replace(discourseStart, "\\1")
            .replace(parenthetical, ",")
            .replace(inOrderTo, "to")
            .replace(atThisPoint, "now")
            .replace(dueTo, "because")
            .replace(forPurpose, "to");
    }

    // Normalize spoken times like "3 30 pm" -> "3:30 PM"
    static const Regex spokenTime = expression(R"(\b(1[0-2]|[1-9])\h+([0-5]\d)\h*(a\.?m\.?|p\.?m\.?)\b)");
    text = substitute(text, spokenTime, [](const auto &m, const QString &) {
        const QString period = m.captured(3).startsWith('a', Qt::CaseInsensitive) ? "AM" : "PM";
        return m.captured(1) + ":" + m.captured(2) + " " + period;
    });

    static const Regex doubleSpaces(R"(\h{2,})");
    static const Regex spacesBeforeMark(R"(\h+([,.!?;:]))");
    static const Regex danglingStart(R"(^\h*[,.;:!?]+\h*)");
    static const Regex doubleComma(R"(,\h*,)");
    static const Regex trailingHoriz(R"(\h+$)");
    text.replace(doubleComma, ",")
        .replace(doubleSpaces, " ")
        .replace(spacesBeforeMark, "\\1")
        .remove(danglingStart)
        .remove(trailingHoriz);
    return text;
}

QString cleanupUniversal(QString text)
{
    // Safe hesitation tokens that are not valid words in German/Hungarian/French/Spanish/etc.
    static const Regex universalFiller = expression(
        R"((?:,\h*)?\b(?:uhm+|umm+|uhh+|erm+|ähm+|euh+|öö+|őő+)\b(?:[,\h]+|(?=\b)))");
    text.replace(universalFiller, " ");
    static const Regex doubleSpaces(R"(\h{2,})");
    static const Regex spacesBeforeMark(R"(\h+([,.!?;:]))");
    text.replace(doubleSpaces, " ").replace(spacesBeforeMark, "\\1");
    return text.trimmed();
}

QString numberedList(QString text)
{
    static const QStringList ordinals = {"first|firstly", "second|secondly", "third|thirdly",
                                          "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth"};
    static const QStringList cardinals = {"one", "two", "three", "four", "five",
                                           "six", "seven", "eight", "nine", "ten"};
    static const Regex start = expression(R"(\b(?:number\h+|point\h+)?(first|firstly|one|1)[,.):]?\h+)");
    static const Regex intro = expression(R"((?:[.!?:]\h*$|\b(?:steps|list|items|tasks|things|points|reasons)(?:\h+are)?[,.:]?\h*$))");
    static const Regex itemStop(R"([,;:\h]+$)");
    for (const auto &m : start.globalMatch(text)) {
        const QString prefix = text.left(m.capturedStart()).trimmed();
        const QString cap = m.captured(1).toLower();
        const bool isOrdinal = cap == "first" || cap == "firstly";
        const bool explicitNumber = m.captured(0).startsWith("number", Qt::CaseInsensitive)
                                    || m.captured(0).startsWith("point", Qt::CaseInsensitive);
        if (!prefix.isEmpty() && !intro.match(prefix).hasMatch() && !explicitNumber) continue;
        QList<qsizetype> ends;
        QList<qsizetype> starts{m.capturedEnd()};
        qsizetype current = m.capturedEnd();
        for (qsizetype n = 1; n < ordinals.size(); ++n) {
            QString indicator = isOrdinal ? ordinals[n] : cardinals[n] + "|" + QString::number(n + 1);
            const auto next = expression(R"(\b(?:number\h+|point\h+)?(?:)" + indicator + R"()[,.)\h:]\h*)").match(text, current);
            if (!next.hasMatch() || next.capturedStart() <= current) break;
            // Plain cardinals need a spoken separator or a list introduction.
            if (!isOrdinal && !explicitNumber && prefix.isEmpty()
                && !text.sliced(current, next.capturedStart() - current).trimmed().contains(Regex("[,.;:]$")))
                break;
            ends.append(next.capturedStart());
            starts.append(next.capturedEnd());
            current = next.capturedEnd();
        }
        if (starts.size() < 2) continue;
        ends.append(text.size());
        QStringList rows;
        if (!prefix.isEmpty()) rows.append(prefix);
        for (qsizetype i = 0; i < starts.size(); ++i) {
            QString item = text.sliced(starts[i], ends[i] - starts[i]).trimmed();
            item.remove(itemStop);
            rows.append(QString::number(i + 1) + ". " + item);
        }
        return rows.join('\n');
    }
    return text;
}

QString styleText(QString text, const QString &style, bool english, const QList<Literal> &literals)
{
    if (english) {
        static const Regex sentenceStart(R"((?:(?:^|\n)(?:[-•]|\d+\.)?\h*|[!?]\h+|\.\h+)["“(]?\K\p{Ll})",
                                         Regex::UseUnicodePropertiesOption);
        static const Regex abbrev = expression(R"(\b(?:e\.g|i\.e|mr|mrs|ms|dr|vs|prof)\.\h+$)");
        text = substitute(text, sentenceStart, [](const auto &m, const QString &source) {
            if (abbrev.match(source.left(m.capturedStart()).right(16)).hasMatch()) return m.captured();
            return m.captured().toUpper();
        });
        static const Regex pronoun = expression(R"(\bi\b)");
        text.replace(pronoun, "I");
    }
    const bool informal = style == "casual" || style == "very casual" || style == "lowercase";
    auto endsWithWordChar = [&] {
        if (text.isEmpty()) return false;
        if (text.back().isLetterOrNumber()) return true;
        for (const Literal &lit : literals) {
            if (text.endsWith(lit.marker))
                return !lit.value.isEmpty() && lit.value.back().isLetterOrNumber();
        }
        return false;
    };
    if (english && !informal && endsWithWordChar())
        text += style == "excited" ? "!" : ".";
    if (style == "excited" && text.endsWith('.') && !text.endsWith("...")) text.back() = '!';
    if (informal && (style != "casual" || text.count(Regex("[.!?]")) <= 10)) {
        static const Regex trailingDots(R"(\.+$)");
        text.remove(trailingDots);
    }
    if (style == "very casual" || style == "lowercase") text = text.toLower();
    return text;
}
} // namespace

QString Formatter::format(const QString &raw, const QString &language, const QString &cleanup,
                          const QString &style, const QVariantList &dictionary,
                          const QVariantList &snippets)
{
    FormatOptions options;
    options.language = language;
    options.cleanup = cleanup;
    options.style = style;
    options.dictionary = dictionary;
    options.snippets = snippets;
    return format(raw, options);
}

QString Formatter::format(const QString &raw, const FormatOptions &options)
{
    const Rules rules(options.dictionary, options.snippets, options.clipboardText);
    QString text = raw.trimmed();
    if (text.isEmpty()) return {};
    const auto standalone = rules.standalone.constFind(looseKey(text));
    if (standalone != rules.standalone.cend()) return standalone.value();

    const QString delimiter = nextDelimiter(text);
    QList<Literal> literals;
    text = maskEntries(text, rules, literals, delimiter);

    const QString mode = key(options.cleanup);
    QString tone = key(options.style);
    tone.replace('-', ' ').replace('_', ' ');
    const bool plain = mode == "none" || tone == "verbatim" || tone == "unformatted";
    const QString lang = options.language.toLower().section(Regex("[-_]"), 0, 0);
    // The recognizer supplies the language it detected when the user selects Auto.
    const bool english = lang == "en" || lang == "english";

    if (!plain && english) {
        text = spokenCodeAndWeb(text, options.codeMode, options.fileTagging,
                                options.contextSymbols, literals, delimiter);
    }

    text = commands(text, plain, english);
    if (!plain) {
        if (english) {
            text = cleanupEnglish(text, mode == "medium");
            text = numberedList(text);
            static const Regex leftSpaces(R"(^\h+)");
            text.remove(leftSpaces);
            if (text.startsWith("\n- ")) text.remove(0, 1);
            text = styleText(text, tone, true, literals);
        } else {
            text = cleanupUniversal(text);
            text = styleText(text, tone, false, literals);
        }
        if (options.noTrailingPeriod && text.endsWith('.') && !text.endsWith("..."))
            text.chop(1);
    }

    // Restore in a single left-to-right pass so order of masking never matters
    // and a literal containing another marker is never expanded again.
    QString restored = text;
    if (!literals.isEmpty()) {
        QHash<QString, QString> byMarker;
        byMarker.reserve(literals.size());
        for (const Literal &literal : literals) byMarker.insert(literal.marker, literal.value);
        const Regex markerRx(Regex::escape(delimiter) + R"(\d+)" + Regex::escape(delimiter));
        QString result;
        qsizetype offset = 0;
        for (const auto &m : markerRx.globalMatch(text)) {
            result += text.sliced(offset, m.capturedStart() - offset);
            result += byMarker.value(m.captured(), m.captured());
            offset = m.capturedEnd();
        }
        restored = result + text.sliced(offset);
    }

    // Never leave a trailing period on URL bars or bare standalone URLs (e.g. "x.com", "github.com").
    if (!plain && !restored.isEmpty()) {
        static const Regex bareUrlRx(
            R"(^(?:https?://|www\.)\S+$|^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+(?:com|org|net|io|ai|dev|app|co|edu|gov|me|tv|gg|xyz|sh|rs|uk|de|fr|hu|eu|ca|jp|nl|ch|se|it|es|pl|cz|sk|ro|at|be|info|biz|site|tech|cloud|so|to|cc|ly|fm|im|in|us|au|nz|kr|cn)(?::\d+)?(?:/\S*)?$)",
            Regex::CaseInsensitiveOption);
        const bool hadTrailingDot = restored.endsWith('.') && !restored.endsWith("...");
        const QString candidate = hadTrailingDot ? restored.left(restored.size() - 1) : restored;
        const bool isBareUrl = bareUrlRx.match(candidate).hasMatch();
        if (hadTrailingDot && (options.noTrailingPeriod || isBareUrl))
            restored = candidate;
        if (isBareUrl) {
            // Keep domain lowercase ("X.com" -> "x.com") while preserving case in any path after '/'.
            const qsizetype schemeEnd = restored.indexOf("://");
            const qsizetype pathStart = restored.indexOf('/', schemeEnd >= 0 ? schemeEnd + 3 : 0);
            if (pathStart < 0) restored = restored.toLower();
            else restored = restored.left(pathStart).toLower() + restored.mid(pathStart);
        }
    }
    return restored;
}

QStringList Formatter::extractSymbols(const QString &text, int limit)
{
    QStringList out;
    QSet<QString> seen;
    auto push = [&](const QString &sym) {
        if (out.size() >= limit || sym.size() < 3 || sym.size() > 64 || seen.contains(sym)) return;
        seen.insert(sym);
        out.append(sym);
    };
    // Filenames with extensions
    static const Regex fileRx(R"(\b[A-Za-z0-9][A-Za-z0-9_-]{0,40}\.(?:cpp|cc|cxx|hpp|h|c|py|qml|js|jsx|ts|tsx|rs|go|java|kt|swift|rb|php|cs|sh|json|yaml|yml|toml|xml|html|css|scss|sql|md|txt|pro|lua|nix|svg)\b)");
    for (const auto &m : fileRx.globalMatch(text)) push(m.captured());
    // camelCase, PascalCase, snake_case identifiers
    static const Regex identRx(R"(\b(?:[a-z]+[A-Z][A-Za-z0-9]*|[A-Z][a-z]+[A-Z][A-Za-z0-9]*|[a-zA-Z][a-zA-Z0-9]*_[a-zA-Z0-9_]+)\b)");
    for (const auto &m : identRx.globalMatch(text)) push(m.captured());
    return out;
}
