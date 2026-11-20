#include "hudwindow.h"

#include <QClipboard>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kCanvasWidth = 380;
constexpr int kCanvasHeight = 58;
constexpr qreal kMonoHeight = 38.0;
constexpr qreal kGlassHeight = 40.0;
constexpr qreal kEditHeight = 44.0;
constexpr qreal kEditWidth = 356.0;
constexpr double kBarWeights[9] = {0.28, 0.54, 0.82, 0.96, 1.00, 0.92, 0.78, 0.50, 0.28};

QColor lerpColor(const QColor &a, const QColor &b, qreal t)
{
    const qreal u = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(
        a.redF() + (b.redF() - a.redF()) * u,
        a.greenF() + (b.greenF() - a.greenF()) * u,
        a.blueF() + (b.blueF() - a.blueF()) * u,
        a.alphaF() + (b.alphaF() - a.alphaF()) * u);
}

int findRuleSeparator(const QString &text, int &sepLen)
{
    for (const QString &sep : {QStringLiteral("->"), QStringLiteral("=>"), QStringLiteral("="), QStringLiteral(":")}) {
        const int idx = text.indexOf(sep);
        if (idx >= 0) {
            sepLen = sep.size();
            return idx;
        }
    }
    sepLen = 0;
    return -1;
}

void drawTextWithOptionalShadow(QPainter &p, const QRectF &rect, int flags, const QString &text,
                                const QColor &color, bool glassShadow)
{
    if (glassShadow) {
        p.setPen(QColor(0, 0, 0, 145));
        p.drawText(rect.translated(0.0, 0.9), flags, text);
    }
    p.setPen(color);
    p.drawText(rect, flags, text);
}
} // namespace

HudRule HudWindow::parseRule(const QString &input)
{
    HudRule rule;
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return rule;

    if (trimmed.startsWith('-')) {
        const QString target = trimmed.mid(1).trimmed();
        if (!target.isEmpty()) {
            rule.action = HudRule::Action::Forget;
            rule.trigger = target;
        }
        return rule;
    }

    int sepLen = 0;
    const int sepIdx = findRuleSeparator(trimmed, sepLen);
    if (sepIdx >= 0) {
        const QString left = trimmed.left(sepIdx).trimmed();
        const QString right = trimmed.mid(sepIdx + sepLen).trimmed();
        if (!left.isEmpty() && !right.isEmpty()) {
            rule.action = HudRule::Action::Learn;
            rule.trigger = left;
            rule.value = right;
        }
        return rule;
    }

    rule.action = HudRule::Action::Learn;
    rule.trigger = trimmed;
    rule.value = trimmed;
    return rule;
}

HudWindow::HudWindow(QWindow *parent)
    : QRasterWindow(parent)
{
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);
    resize(kCanvasWidth, kCanvasHeight);
    setMinimumSize(QSize(kCanvasWidth, kCanvasHeight));
    setMaximumSize(QSize(kCanvasWidth, kCanvasHeight));
    setFlags(Qt::FramelessWindowHint);

    m_animTimer.setInterval(16); // ~60 fps smooth animation
    connect(&m_animTimer, &QTimer::timeout, this, &HudWindow::tick);
}

void HudWindow::setTheme(const QString &theme)
{
    const QString clean = (theme == "mono") ? QStringLiteral("mono") : QStringLiteral("glass");
    if (m_theme == clean) return;
    m_theme = clean;
    update();
}

bool HudWindow::shouldBeVisible() const
{
    return m_editing || (m_enabled && (m_phase != "idle" || !m_toast.isEmpty()));
}

bool HudWindow::isClickablePill() const
{
    return m_enabled && (m_phase != "idle" || (!m_toast.isEmpty() && m_toastKind != "error"));
}

bool HudWindow::isInteractive() const
{
    return m_editing || isClickablePill();
}

qreal HudWindow::targetPillWidth() const
{
    if (m_editing) return kEditWidth;
    if (m_phase == "idle" && !m_toast.isEmpty()) {
        QFont f;
        f.setPixelSize(12);
        f.setWeight(QFont::Medium);
        const qreal tw = QFontMetricsF(f).horizontalAdvance(m_toast);
        const qreal extra = (m_hovered && m_toastKind == "ok") ? 58.0 : 0.0;
        return std::clamp(tw + 32.0 + extra, 84.0, 356.0);
    }
    if (m_phase == "starting") return 132.0;
    if (m_phase == "processing") return isGlass() ? 80.0 : 76.0;
    if (m_phase == "recording") {
        // Ultra-minimal: waveform only (no timer text).
        qreal w = isGlass() ? 108.0 : 102.0;
        if (m_mode == "handsfree") w += 68.0; // cancel + stop buttons
        return w;
    }
    return 108.0;
}

qreal HudWindow::targetPillHeight() const
{
    if (m_editing) return kEditHeight;
    return isGlass() ? kGlassHeight : kMonoHeight;
}

QRectF HudWindow::pillRect(const QSize &canvas) const
{
    const qreal slideY = (1.0 - m_appear) * 6.0;
    return QRectF((canvas.width() - m_animWidth) * 0.5,
                  canvas.height() - m_animHeight - 5.0 + slideY,
                  m_animWidth, m_animHeight);
}

QRectF HudWindow::cancelButtonRect(const QRectF &pill) const
{
    return QRectF(pill.left() + 8.0, pill.center().y() - 12.0, 24.0, 24.0);
}

QRectF HudWindow::stopButtonRect(const QRectF &pill) const
{
    return QRectF(pill.right() - 32.0, pill.center().y() - 12.0, 24.0, 24.0);
}

void HudWindow::applyState(const QJsonObject &msg)
{
    if (msg.contains("theme"))
        setTheme(msg.value("theme").toString("glass"));

    if (msg.contains("editing")) {
        const bool nextEdit = msg.value("editing").toBool();
        if (m_editing != nextEdit) {
            m_editing = nextEdit;
            if (!m_editing) {
                m_hadFocus = false;
                m_selectAll = false;
                setCursor(Qt::ArrowCursor);
            }
            emit editingChanged(m_editing);
        }
        if (msg.contains("editText")) {
            m_editText = msg.value("editText").toString();
            m_cursorPos = m_editText.size();
        }
    }
    if (m_editing && !msg.contains("editing")) return;

    const bool wasVisible = shouldBeVisible();
    m_phase = msg.value("phase").toString("idle");
    m_mode = msg.value("mode").toString("ptt");
    m_enabled = msg.value("hud").toBool(true);
    m_level = std::clamp(msg.value("level").toDouble(), 0.0, 1.0);
    m_whisper = msg.value("whisper").toBool();
    m_toast = msg.value("toast").toString();
    m_toastKind = msg.value("toastKind").toString("info");
    if (msg.contains("waveTick")) m_waveTick = msg.value("waveTick").toInt();

    const bool snap = msg.value("snap").toBool();
    if (snap) {
        m_animWidth = targetPillWidth();
        m_animHeight = targetPillHeight();
        m_editMorph = m_editing ? 1.0 : 0.0;
        m_appear = 1.0;
        m_smoothLevel = m_level;
    } else if (!wasVisible && shouldBeVisible()) {
        m_animWidth = targetPillWidth() * 0.90;
        m_animHeight = targetPillHeight();
        m_editMorph = m_editing ? 1.0 : 0.0;
        m_appear = 0.0;
        m_smoothLevel = m_level;
    }
    updateAnimationTimer();
    update();
}

void HudWindow::enterEditMode()
{
    if (m_editing) return;
    m_editing = true;
    m_hadFocus = false;
    m_selectAll = false;
    m_editText.clear();
    m_cursorPos = 0;
    m_blinkTick = 0;
    m_appear = 1.0;
    m_hoverCancel = m_hoverStop = false;
    setCursor(Qt::IBeamCursor);
    emit editingChanged(true);
    emit messageTriggered({{"action", "hud-edit-start"}});
    updateAnimationTimer();
    update();
}

void HudWindow::exitEditMode(bool notifyCancel)
{
    if (!m_editing) return;
    m_editing = false;
    m_hadFocus = false;
    m_selectAll = false;
    m_editText.clear();
    m_cursorPos = 0;
    setCursor(Qt::ArrowCursor);
    emit editingChanged(false);
    if (notifyCancel) {
        m_phase = "idle";
        m_toast.clear();
        emit messageTriggered({{"action", "hud-edit-cancel"}});
    }
    updateAnimationTimer();
    update();
}

void HudWindow::submitEdit()
{
    const HudRule rule = parseRule(m_editText);
    if (rule.action == HudRule::Action::Empty) {
        exitEditMode(true);
        return;
    }
    exitEditMode(false);
    if (rule.action == HudRule::Action::Learn) {
        emit messageTriggered({
            {"action", "hud-learn"},
            {"trigger", rule.trigger},
            {"value", rule.value},
        });
    } else if (rule.action == HudRule::Action::Forget) {
        emit messageTriggered({
            {"action", "hud-forget"},
            {"trigger", rule.trigger},
        });
    }
}

void HudWindow::updateAnimationTimer()
{
    const bool animatingState = (m_phase == "starting" || m_phase == "recording" || m_phase == "processing");
    const qreal targetMorph = m_editing ? 1.0 : 0.0;
    const bool settling = std::abs(m_animWidth - targetPillWidth()) > 0.35
                          || std::abs(m_animHeight - targetPillHeight()) > 0.25
                          || std::abs(m_editMorph - targetMorph) > 0.01
                          || m_appear < 0.99;
    if (shouldBeVisible() && (animatingState || m_editing || settling)) {
        if (!m_animTimer.isActive()) m_animTimer.start();
    } else {
        m_animTimer.stop();
    }
}

void HudWindow::tick()
{
    m_waveTick = (m_waveTick + 1) % 1440;
    m_blinkTick = (m_blinkTick + 1) % 66;
    m_smoothLevel += (m_level - m_smoothLevel) * 0.34;

    if (m_appear < 1.0) {
        m_appear += (1.0 - m_appear) * 0.28;
        if (1.0 - m_appear <= 0.01) m_appear = 1.0;
    }

    const qreal tw = targetPillWidth();
    const qreal th = targetPillHeight();
    m_animWidth += (tw - m_animWidth) * 0.24;
    m_animHeight += (th - m_animHeight) * 0.24;
    if (std::abs(m_animWidth - tw) <= 0.35) m_animWidth = tw;
    if (std::abs(m_animHeight - th) <= 0.25) m_animHeight = th;

    const qreal targetMorph = m_editing ? 1.0 : 0.0;
    m_editMorph += (targetMorph - m_editMorph) * 0.24;
    if (std::abs(m_editMorph - targetMorph) <= 0.01) m_editMorph = targetMorph;

    updateAnimationTimer();
    update();
}

void HudWindow::renderToPainter(QPainter &p, const QSize &canvas) const
{
    if (!shouldBeVisible()) return;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setOpacity(m_appear);

    const bool glass = isGlass();
    const QRectF pill = pillRect(canvas);
    const qreal radius = pill.height() * 0.5;
    const bool isToast = (!m_editing && m_phase == "idle" && !m_toast.isEmpty());

    // -------------------------------------------------------------------------
    // Pill Background Surface
    // -------------------------------------------------------------------------
    if (!glass) {
        // Simple Black & White: pure solid #000000 black pill, zero border in all states.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 255));
        p.drawRoundedRect(pill, radius, radius);
    } else {
        // Frosted Liquid Glass: translucent smoked-glass fill lets compositor Kawase blur
        // match whatever window or wallpaper sits behind the pill.
        // Everything stays clipped cleanly inside pillPath so Hyprland's ignore_alpha=0.08
        // masks the blur to the exact rounded capsule with zero outer artifacts.
        QPainterPath pillPath;
        pillPath.addRoundedRect(pill, radius, radius);

        p.save();
        p.setClipPath(pillPath);

        QLinearGradient bodyGrad(pill.topLeft(), pill.bottomLeft());
        if (m_editMorph > 0.02) {
            bodyGrad.setColorAt(0.0, lerpColor(QColor(24, 27, 34, 102), QColor(16, 18, 24, 158), m_editMorph));
            bodyGrad.setColorAt(0.50, lerpColor(QColor(14, 16, 22, 84), QColor(12, 14, 20, 144), m_editMorph));
            bodyGrad.setColorAt(1.0, lerpColor(QColor(18, 20, 27, 108), QColor(16, 18, 24, 164), m_editMorph));
        } else {
            bodyGrad.setColorAt(0.0, QColor(24, 27, 34, 102));
            bodyGrad.setColorAt(0.50, QColor(14, 16, 22, 84));
            bodyGrad.setColorAt(1.0, QColor(18, 20, 27, 108));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(bodyGrad);
        p.drawRect(pill);

        // Top specular liquid-glass dome highlight (crisp catchlight along upper curve).
        QLinearGradient topCatchlight(pill.topLeft(), QPointF(pill.left(), pill.top() + pill.height() * 0.52));
        topCatchlight.setColorAt(0.0, QColor(255, 255, 255, 62));
        topCatchlight.setColorAt(0.42, QColor(255, 255, 255, 16));
        topCatchlight.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setBrush(topCatchlight);
        p.drawRect(QRectF(pill.left(), pill.top(), pill.width(), pill.height() * 0.52));

        // Subtle ambient bottom internal refraction.
        QLinearGradient bottomBounce(QPointF(pill.left(), pill.bottom() - pill.height() * 0.30), pill.bottomLeft());
        bottomBounce.setColorAt(0.0, QColor(255, 255, 255, 0));
        bottomBounce.setColorAt(1.0, QColor(255, 255, 255, 24));
        p.setBrush(bottomBounce);
        p.drawRect(QRectF(pill.left(), pill.bottom() - pill.height() * 0.30, pill.width(), pill.height() * 0.30));

        p.restore();

        // Crisp directional liquid-glass rim highlight (single clean bevel, no double ring).
        QColor topRim(255, 255, 255, m_hovered || m_editing ? 195 : 162);
        QColor midRim(255, 255, 255, m_editing ? 68 : 38);
        QColor botRim(255, 255, 255, m_editing ? 115 : 88);
        qreal rimWidth = m_editMorph > 0.3 ? 1.25 : 1.05;
        if (isToast && m_toastKind == "ok") {
            topRim = QColor(126, 244, 164, 235);
            midRim = QColor(92, 226, 138, 185);
            botRim = QColor(112, 236, 152, 215);
            rimWidth = 1.35;
        } else if (isToast && m_toastKind == "error") {
            topRim = QColor(255, 142, 126, 230);
            midRim = QColor(242, 114, 98, 175);
            botRim = QColor(246, 126, 110, 205);
            rimWidth = 1.35;
        } else if (m_phase == "recording" && m_whisper) {
            topRim = QColor(185, 228, 255, 180);
            botRim = QColor(152, 208, 248, 105);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 40), 0.8));
        p.drawRoundedRect(pill.adjusted(0.4, 0.4, -0.4, -0.4), radius - 0.4, radius - 0.4);

        QLinearGradient rimGrad(pill.topLeft(), pill.bottomLeft());
        rimGrad.setColorAt(0.0, topRim);
        rimGrad.setColorAt(0.46, midRim);
        rimGrad.setColorAt(1.0, botRim);
        p.setPen(QPen(QBrush(rimGrad), rimWidth));
        p.drawRoundedRect(pill.adjusted(0.85, 0.85, -0.85, -0.85), radius - 0.85, radius - 0.85);
    }

    QFont font;
    font.setStyleHint(QFont::SansSerif);
    font.setWeight(QFont::Medium);

    // -------------------------------------------------------------------------
    // Morphing Inline Typing Bar ("arm -> ARM", "slash = /")
    // -------------------------------------------------------------------------
    if (m_editMorph > 0.15) {
        p.save();
        p.setOpacity(m_appear * std::clamp((m_editMorph - 0.15) / 0.85, 0.0, 1.0));

        // Prompt chevron '>'
        QFont chevronFont = font;
        chevronFont.setPixelSize(13);
        chevronFont.setWeight(QFont::Bold);
        p.setFont(chevronFont);
        drawTextWithOptionalShadow(p, QRectF(pill.left() + 15.0, pill.top(), 14.0, pill.height()),
                                   Qt::AlignVCenter, ">",
                                   glass ? QColor(255, 255, 255, 225) : QColor("#ffffff"), glass);

        // Right "Enter" keycap
        const HudRule preview = parseRule(m_editText);
        const bool validRule = (preview.action != HudRule::Action::Empty);
        QFont badgeFont = font;
        badgeFont.setPixelSize(10);
        badgeFont.setWeight(QFont::DemiBold);
        const qreal badgeW = 44.0;
        const QRectF badge(pill.right() - badgeW - 11.0, pill.center().y() - 10.5, badgeW, 21.0);
        if (!glass) {
            p.setPen(Qt::NoPen);
            p.setBrush(validRule ? QColor("#ffffff") : QColor("#1f1f1f"));
            p.drawRoundedRect(badge, 6.0, 6.0);
            p.setFont(badgeFont);
            p.setPen(validRule ? QColor("#000000") : QColor("#8a8a8a"));
            p.drawText(badge, Qt::AlignCenter, QStringLiteral("Enter"));
        } else {
            p.setPen(validRule ? Qt::NoPen : QPen(QColor(255, 255, 255, 65), 1.0));
            p.setBrush(validRule ? QColor(255, 255, 255, 232) : QColor(255, 255, 255, 28));
            p.drawRoundedRect(badge, 6.5, 6.5);
            p.setFont(badgeFont);
            if (validRule) {
                p.setPen(QColor(14, 16, 22, 245));
                p.drawText(badge, Qt::AlignCenter, QStringLiteral("Enter"));
            } else {
                drawTextWithOptionalShadow(p, badge, Qt::AlignCenter, QStringLiteral("Enter"),
                                           QColor(255, 255, 255, 175), true);
            }
        }

        // Input field region
        const QRectF fieldRect(pill.left() + 31.0, pill.top() + 2.0,
                               badge.left() - (pill.left() + 31.0) - 8.0, pill.height() - 4.0);
        p.setClipRect(fieldRect);

        QFont inputFont = font;
        inputFont.setPixelSize(13);
        p.setFont(inputFont);
        const QFontMetricsF fm(inputFont);

        if (m_editText.isEmpty()) {
            drawTextWithOptionalShadow(p, fieldRect, Qt::AlignVCenter | Qt::AlignLeft,
                                       QStringLiteral("arm -> ARM"),
                                       glass ? QColor(255, 255, 255, 130) : QColor("#6e6e6e"), glass);
            if (m_blinkTick < 36) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#ffffff"));
                p.drawRoundedRect(QRectF(fieldRect.left() + 0.5, pill.center().y() - 8.5, 1.8, 17.0), 0.9, 0.9);
            }
        } else {
            const qreal cursorXRaw = fm.horizontalAdvance(m_editText.left(m_cursorPos));
            const qreal scrollX = std::max(0.0, cursorXRaw - fieldRect.width() + 8.0);
            const qreal baseX = fieldRect.left() - scrollX;

            if (m_selectAll) {
                const qreal allW = fm.horizontalAdvance(m_editText);
                p.setPen(Qt::NoPen);
                p.setBrush(glass ? QColor(255, 255, 255, 60) : QColor(255, 255, 255, 70));
                p.drawRoundedRect(QRectF(baseX, pill.center().y() - 9.0, allW + 2.0, 18.0), 3.0, 3.0);
            }

            int sepLen = 0;
            const int sepIdx = findRuleSeparator(m_editText, sepLen);
            if (sepIdx >= 0) {
                const QString leftPart = m_editText.left(sepIdx);
                const QString sepPart = m_editText.mid(sepIdx, sepLen);
                const QString rightPart = m_editText.mid(sepIdx + sepLen);
                qreal x = baseX;
                drawTextWithOptionalShadow(p, QRectF(x, fieldRect.top(), fm.horizontalAdvance(leftPart) + 2.0, fieldRect.height()),
                                           Qt::AlignVCenter | Qt::AlignLeft, leftPart, QColor("#ffffff"), glass);
                x += fm.horizontalAdvance(leftPart);
                drawTextWithOptionalShadow(p, QRectF(x, fieldRect.top(), fm.horizontalAdvance(sepPart) + 2.0, fieldRect.height()),
                                           Qt::AlignVCenter | Qt::AlignLeft, sepPart,
                                           glass ? QColor(255, 255, 255, 170) : QColor("#8f8f8f"), glass);
                x += fm.horizontalAdvance(sepPart);
                drawTextWithOptionalShadow(p, QRectF(x, fieldRect.top(), fm.horizontalAdvance(rightPart) + 2.0, fieldRect.height()),
                                           Qt::AlignVCenter | Qt::AlignLeft, rightPart,
                                           glass ? QColor("#d0ffe0") : QColor("#ffffff"), glass);
            } else {
                const QColor textCol = m_editText.trimmed().startsWith('-')
                    ? (glass ? QColor("#ffaaa0") : QColor("#b0b0b0"))
                    : QColor("#ffffff");
                drawTextWithOptionalShadow(p, QRectF(baseX, fieldRect.top(), fm.horizontalAdvance(m_editText) + 4.0, fieldRect.height()),
                                           Qt::AlignVCenter | Qt::AlignLeft, m_editText, textCol, glass);
            }

            if (m_blinkTick < 36) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor("#ffffff"));
                p.drawRoundedRect(QRectF(baseX + cursorXRaw, pill.center().y() - 8.5, 1.8, 17.0), 0.9, 0.9);
            }
        }
        p.restore();
        return;
    }

    // -------------------------------------------------------------------------
    // Standard Pill States
    // -------------------------------------------------------------------------
    p.save();
    if (m_editMorph > 0.0) p.setOpacity(m_appear * std::max(0.0, 1.0 - m_editMorph * 5.0));

    // 1. Starting
    if (m_phase == "starting") {
        font.setPixelSize(12);
        p.setFont(font);
        const QString text = tr("Starting...");
        const qreal tw = QFontMetricsF(font).horizontalAdvance(text);
        const qreal startX = pill.center().x() - (7.0 + 9.0 + tw) * 0.5;
        const qreal alpha = 0.42 + 0.58 * std::abs(std::sin(m_waveTick * 0.07));
        QColor dot("#ffffff");
        dot.setAlphaF(alpha);
        p.setPen(Qt::NoPen);
        if (glass) {
            QColor glow(255, 255, 255, static_cast<int>(70 * alpha));
            p.setBrush(glow);
            p.drawEllipse(QRectF(startX - 2.5, pill.center().y() - 6.0, 12.0, 12.0));
        }
        p.setBrush(dot);
        p.drawEllipse(QRectF(startX, pill.center().y() - 3.5, 7.0, 7.0));
        drawTextWithOptionalShadow(p, QRectF(startX + 16.0, pill.top(), tw + 4.0, pill.height()),
                                   Qt::AlignVCenter, text, QColor("#ffffff"), glass);
        p.restore();
        return;
    }

    // 2. Processing / Transcribing — minimal animated three-dot wave only (no label text)
    if (m_phase == "processing") {
        const qreal dotSize = 5.4;
        const qreal dotGap = 6.2;
        const qreal dotsW = 3 * dotSize + 2 * dotGap;
        const qreal startX = pill.center().x() - dotsW * 0.5;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            const double b = std::max(0.0, std::sin(m_waveTick * 0.11 - i * 0.95));
            const qreal dx = startX + i * (dotSize + dotGap);
            const qreal dy = pill.center().y() - dotSize * 0.5 - 2.6 * b;
            QColor c("#ffffff");
            c.setAlphaF(0.38 + 0.62 * b);
            if (glass) {
                p.setBrush(QColor(0, 0, 0, static_cast<int>(58 * (0.4 + 0.6 * b))));
                p.drawEllipse(QRectF(dx - 0.5, dy + 0.8, dotSize + 1.0, dotSize + 1.0));
                p.setBrush(QColor(255, 255, 255, static_cast<int>(65 * b)));
                p.drawEllipse(QRectF(dx - 2.1, dy - 2.1, dotSize + 4.2, dotSize + 4.2));
            }
            p.setBrush(c);
            p.drawEllipse(QRectF(dx, dy, dotSize, dotSize));
        }
        p.restore();
        return;
    }

    // 3. Completion / error toast — clean centered text, status conveyed via border color (no sphere)
    if (isToast) {
        font.setPixelSize(12);
        p.setFont(font);
        const QFontMetricsF fm(font);
        const bool showHint = (m_hovered && m_toastKind == "ok" && pill.width() > 132.0);
        const QString hintText = QStringLiteral("fix word");
        const qreal hintW = showHint ? (fm.horizontalAdvance(hintText) + 18.0) : 0.0;
        const QString elided = fm.elidedText(m_toast, Qt::ElideRight, pill.width() - 28.0 - hintW);
        const qreal tw = fm.horizontalAdvance(elided);
        const qreal startX = pill.center().x() - (tw + hintW) * 0.5;

        drawTextWithOptionalShadow(p, QRectF(startX, pill.top(), tw + 4.0, pill.height()),
                                   Qt::AlignVCenter, elided, QColor("#ffffff"), glass);
        if (showHint) {
            const qreal hx = startX + tw + 9.0;
            p.setPen(glass ? QColor(255, 255, 255, 70) : QColor("#3a3a3a"));
            p.drawLine(QPointF(hx, pill.center().y() - 5.5), QPointF(hx, pill.center().y() + 5.5));
            drawTextWithOptionalShadow(p, QRectF(hx + 8.0, pill.top(), hintW, pill.height()),
                                       Qt::AlignVCenter, hintText,
                                       glass ? QColor(255, 255, 255, 210) : QColor("#a8a8a8"), glass);
        }
        p.restore();
        return;
    }

    // 4. Recording — Ultra-minimal: reactive vertical waveform bars ONLY (no timer text)
    if (m_phase == "recording") {
        if (m_mode == "handsfree") {
            const QRectF cBtn = cancelButtonRect(pill);
            p.setPen(Qt::NoPen);
            if (!glass) {
                p.setBrush(m_hoverCancel ? QColor("#2c2c2c") : QColor("#181818"));
                p.drawEllipse(cBtn);
                p.setPen(QPen(QColor("#ffffff"), 1.6, Qt::SolidLine, Qt::RoundCap));
            } else {
                p.setBrush(m_hoverCancel ? QColor(255, 255, 255, 58) : QColor(255, 255, 255, 26));
                p.drawEllipse(cBtn);
                p.setPen(QPen(QColor(255, 255, 255, 225), 1.6, Qt::SolidLine, Qt::RoundCap));
            }
            const QPointF cc = cBtn.center();
            p.drawLine(cc + QPointF(-3.4, -3.4), cc + QPointF(3.4, 3.4));
            p.drawLine(cc + QPointF(3.4, -3.4), cc + QPointF(-3.4, 3.4));

            const QRectF sBtn = stopButtonRect(pill);
            p.setPen(Qt::NoPen);
            if (!glass) {
                p.setBrush(m_hoverStop ? QColor("#dcdcdc") : QColor("#ffffff"));
                p.drawEllipse(sBtn);
                p.setBrush(QColor("#000000"));
                p.drawRoundedRect(QRectF(sBtn.center().x() - 3.8, sBtn.center().y() - 3.8, 7.6, 7.6), 1.4, 1.4);
            } else {
                p.setBrush(m_hoverStop ? QColor(255, 130, 112, 225) : QColor(238, 108, 90, 200));
                p.drawEllipse(sBtn);
                p.setBrush(QColor("#ffffff"));
                p.drawRoundedRect(QRectF(sBtn.center().x() - 3.6, sBtn.center().y() - 3.6, 7.2, 7.2), 1.4, 1.4);
            }
        }

        // Centered 9-bar reactive vertical waveform
        const qreal barW = 2.8;
        const qreal gap = 2.8;
        const qreal waveW = 9 * barW + 8 * gap;
        qreal x = pill.center().x() - waveW * 0.5;
        const double amp = std::sqrt(std::clamp(m_smoothLevel, 0.0, 1.0));
        const QColor coreColor = (glass && m_whisper) ? QColor("#d6efff") : QColor("#ffffff");
        p.setPen(Qt::NoPen);

        for (int i = 0; i < 9; ++i) {
            const double wave = 0.72 + 0.28 * std::sin(m_waveTick * 0.12 + i * 0.85);
            const qreal h = std::clamp(kBarWeights[i] * (5.5 + amp * 18.5 * wave), 4.0, 23.0);
            const QRectF barRect(x, pill.center().y() - h * 0.5, barW, h);
            if (glass) {
                // Soft dark backing shadow + luminous frosted glow so bars stay crisp on bright or dark backgrounds.
                p.setBrush(QColor(0, 0, 0, 58));
                p.drawRoundedRect(barRect.adjusted(-0.8, 0.4, 0.8, 1.2), 2.2, 2.2);
                QColor glow = coreColor;
                glow.setAlpha(60);
                p.setBrush(glow);
                p.drawRoundedRect(barRect.adjusted(-1.4, -1.2, 1.4, 1.2), 2.6, 2.6);
            }
            p.setBrush(coreColor);
            p.drawRoundedRect(barRect, barW * 0.5, barW * 0.5);
            x += barW + gap;
        }
    }
    p.restore();
}

QImage HudWindow::renderPreviewImage() const
{
    QImage img(kCanvasWidth, kCanvasHeight, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    renderToPainter(painter, img.size());
    painter.end();
    return img;
}

void HudWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    renderToPainter(p, size());
}

void HudWindow::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    const QRectF pill = pillRect(size());
    const bool prevHover = m_hovered;
    const bool prevCancel = m_hoverCancel;
    const bool prevStop = m_hoverStop;

    m_hovered = pill.contains(pos);
    const bool hf = (!m_editing && m_phase == "recording" && m_mode == "handsfree");
    m_hoverCancel = hf && cancelButtonRect(pill).contains(pos);
    m_hoverStop = hf && stopButtonRect(pill).contains(pos);

    if (m_editing)
        setCursor(m_hovered ? Qt::IBeamCursor : Qt::ArrowCursor);
    else if (m_hovered && isClickablePill())
        setCursor(Qt::PointingHandCursor);
    else
        setCursor(Qt::ArrowCursor);

    if (m_hovered != prevHover) {
        if (!m_editing && m_phase == "idle" && !m_toast.isEmpty())
            emit messageTriggered({{"action", "hud-hover"}, {"hover", m_hovered}});
        updateAnimationTimer();
        update();
    } else if (m_hoverCancel != prevCancel || m_hoverStop != prevStop) {
        update();
    }
}

void HudWindow::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    const QRectF pill = pillRect(size());
    if (m_editing) {
        if (!pill.contains(pos)) {
            exitEditMode(true);
            return;
        }
        const QRectF badge(pill.right() - 55.0, pill.center().y() - 12.0, 46.0, 24.0);
        if (badge.contains(pos)) {
            submitEdit();
        } else {
            m_selectAll = false;
            m_cursorPos = m_editText.size();
            m_blinkTick = 0;
            update();
        }
        return;
    }

    if (!pill.contains(pos)) return;
    if (m_phase == "recording" && m_mode == "handsfree") {
        if (cancelButtonRect(pill).contains(pos)) {
            emit messageTriggered({{"action", "cancel"}});
            return;
        }
        if (stopButtonRect(pill).contains(pos)) {
            emit messageTriggered({{"action", "stop"}});
            return;
        }
    }
    if (isClickablePill())
        enterEditMode();
}

void HudWindow::keyPressEvent(QKeyEvent *event)
{
    if (!m_editing) return;
    m_blinkTick = 0;

    const int k = event->key();
    const auto mods = event->modifiers();

    if (k == Qt::Key_Escape) {
        exitEditMode(true);
        return;
    }
    if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        submitEdit();
        return;
    }
    if ((mods & Qt::ControlModifier) && k == Qt::Key_A) {
        m_selectAll = !m_editText.isEmpty();
        m_cursorPos = m_editText.size();
        update();
        return;
    }
    if ((mods & Qt::ControlModifier) && k == Qt::Key_V) {
        if (auto *cb = QGuiApplication::clipboard()) {
            const QString pasted = cb->text().simplified();
            if (!pasted.isEmpty()) {
                if (m_selectAll) { m_editText.clear(); m_cursorPos = 0; m_selectAll = false; }
                m_editText.insert(m_cursorPos, pasted);
                m_cursorPos += pasted.size();
                update();
            }
        }
        return;
    }
    if (k == Qt::Key_Backspace) {
        if (m_selectAll) {
            m_editText.clear();
            m_cursorPos = 0;
            m_selectAll = false;
        } else if (m_cursorPos > 0) {
            if (mods & Qt::ControlModifier) {
                int start = m_cursorPos;
                while (start > 0 && m_editText[start - 1].isSpace()) --start;
                while (start > 0 && !m_editText[start - 1].isSpace()) --start;
                m_editText.remove(start, m_cursorPos - start);
                m_cursorPos = start;
            } else {
                m_editText.remove(m_cursorPos - 1, 1);
                --m_cursorPos;
            }
        }
        update();
        return;
    }
    if (k == Qt::Key_Delete) {
        if (m_selectAll) {
            m_editText.clear();
            m_cursorPos = 0;
            m_selectAll = false;
        } else if (m_cursorPos < m_editText.size()) {
            m_editText.remove(m_cursorPos, 1);
        }
        update();
        return;
    }
    if (k == Qt::Key_Left) {
        m_selectAll = false;
        if (m_cursorPos > 0) --m_cursorPos;
        update();
        return;
    }
    if (k == Qt::Key_Right) {
        m_selectAll = false;
        if (m_cursorPos < m_editText.size()) ++m_cursorPos;
        update();
        return;
    }
    if (k == Qt::Key_Home) {
        m_selectAll = false;
        m_cursorPos = 0;
        update();
        return;
    }
    if (k == Qt::Key_End) {
        m_selectAll = false;
        m_cursorPos = m_editText.size();
        update();
        return;
    }

    const QString txt = event->text();
    if (!txt.isEmpty() && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        QString printable;
        for (const QChar ch : txt) {
            if (ch.isPrint()) printable.append(ch);
        }
        if (!printable.isEmpty()) {
            if (m_selectAll) {
                m_editText.clear();
                m_cursorPos = 0;
                m_selectAll = false;
            }
            m_editText.insert(m_cursorPos, printable);
            m_cursorPos += printable.size();
            update();
        }
    }
}

bool HudWindow::event(QEvent *ev)
{
    if (ev->type() == QEvent::Leave) {
        const bool wasHovered = m_hovered;
        m_hovered = m_hoverCancel = m_hoverStop = false;
        if (wasHovered && !m_editing && m_phase == "idle" && !m_toast.isEmpty())
            emit messageTriggered({{"action", "hud-hover"}, {"hover", false}});
        updateAnimationTimer();
        update();
    } else if (ev->type() == QEvent::FocusIn && m_editing) {
        m_hadFocus = true;
    } else if (ev->type() == QEvent::FocusOut && m_editing && m_hadFocus) {
        exitEditMode(true);
    }
    return QRasterWindow::event(ev);
}
