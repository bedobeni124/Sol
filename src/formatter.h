#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

struct FormatOptions {
    QString language = "en";
    QString cleanup = "light"; // none, light, medium
    QString style = "formal";  // formal, casual, lowercase, excited, verbatim
    bool codeMode = false;
    bool fileTagging = false;
    bool noTrailingPeriod = false;
    QStringList contextSymbols;
    QString clipboardText;
    QVariantList dictionary;
    QVariantList snippets;
};

class Formatter {
public:
    static QString format(const QString &raw, const QString &language, const QString &cleanup,
                          const QString &style, const QVariantList &dictionary,
                          const QVariantList &snippets);
    static QString format(const QString &raw, const FormatOptions &options);
    // Extract camelCase, PascalCase, snake_case, and filename identifiers from context text.
    static QStringList extractSymbols(const QString &text, int limit = 60);
};
