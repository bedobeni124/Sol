#pragma once

#include "backend.h"
#include "desktop.h"

#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <functional>

namespace CliUi {

bool useColor();

// Semantic highlighting: Good (green), Important (gold/cyan), Bad (red), Warn (yellow), Dim (muted)
QString good(const QString &text);
QString bad(const QString &text);
QString warn(const QString &text);
QString imp(const QString &text);
QString key(const QString &text);
QString cmd(const QString &text);
QString strong(const QString &text);
QString dim(const QString &text);

QString badgeOnOff(bool value);
QString badgeGood(const QString &label);
QString badgeBad(const QString &label);
QString badgeWarn(const QString &label);
QString badgeImp(const QString &label);

void printBanner(QTextStream &out, const QString &view = {});
void printSection(QTextStream &out, const QString &title);
void printRow(QTextStream &out, const QString &marker, const QString &label,
              const QString &value, const QString &hint = {});

void printHelp(QTextStream &out);
void printStatusCard(QTextStream &out, const QJsonObject &status, bool daemonRunning);
void printDoctor(QTextStream &out, const QJsonObject &probe);
void printSettingsSnapshot(QTextStream &out, const Backend &b);
void printStyleOverview(QTextStream &out, const Backend &b);
void printStats(QTextStream &out, const Backend &b);
void printEntries(QTextStream &out, const QString &kind, const QVariantList &rows);
void printHistory(QTextStream &out, const QVariantList &rows, int limit, const QString &search = {});

bool interactiveLanguageSetup(Backend &b, QTextStream &out,
                              const std::function<void()> &reloadDaemon);
bool interactiveHudThemeSetup(Backend &b, QTextStream &out,
                              const std::function<void()> &reloadDaemon);
bool interactiveShortcutSetup(Backend &b, Desktop &d, QTextStream &out,
                              const std::function<void()> &reloadDaemon,
                              const std::function<void()> &portalBind);
void interactiveSettings(Backend &b, Desktop &d, QTextStream &out,
                         const std::function<void()> &reloadDaemon,
                         const std::function<void()> &portalBind);
int runMicTest(const QString &microphone, QTextStream &out, QTextStream &err);

} // namespace CliUi
