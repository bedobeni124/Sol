#pragma once

#include <QImage>
#include <QJsonObject>
#include <QRasterWindow>
#include <QRectF>
#include <QString>
#include <QTimer>

struct HudRule {
    enum class Action { Empty, Learn, Forget };
    Action action = Action::Empty;
    QString trigger;
    QString value;
};

class HudWindow : public QRasterWindow {
    Q_OBJECT
public:
    explicit HudWindow(QWindow *parent = nullptr);

    void setTheme(const QString &theme);
    QString theme() const { return m_theme; }

    void applyState(const QJsonObject &msg);
    void renderToPainter(QPainter &painter, const QSize &canvasSize) const;
    QImage renderPreviewImage() const;

    bool shouldBeVisible() const;
    bool isInteractive() const;
    bool isEditing() const { return m_editing; }

    static HudRule parseRule(const QString &input);

signals:
    void messageTriggered(const QJsonObject &msg);
    void editingChanged(bool editing);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *ev) override;

private:
    void enterEditMode();
    void exitEditMode(bool notifyCancel);
    void submitEdit();
    void tick();
    void updateAnimationTimer();
    bool isClickablePill() const;
    bool isGlass() const { return m_theme == "glass"; }
    QRectF pillRect(const QSize &canvas) const;
    QRectF cancelButtonRect(const QRectF &pill) const;
    QRectF stopButtonRect(const QRectF &pill) const;
    qreal targetPillWidth() const;
    qreal targetPillHeight() const;

    QString m_theme = "glass"; // "mono" | "glass"
    QString m_phase = "idle";
    QString m_mode = "ptt";
    QString m_toast;
    QString m_toastKind = "info";
    bool m_enabled = true;
    bool m_whisper = false;
    bool m_hovered = false;
    bool m_hoverCancel = false;
    bool m_hoverStop = false;
    bool m_editing = false;
    bool m_hadFocus = false;
    bool m_selectAll = false;
    QString m_editText;
    int m_cursorPos = 0;
    qreal m_editMorph = 0.0; // 0 = pill, 1 = expanded typing bar
    qreal m_appear = 1.0;    // 0 -> 1 smooth entrance glide
    qreal m_level = 0.0;
    qreal m_smoothLevel = 0.0;
    qreal m_animWidth = 112.0;
    qreal m_animHeight = 38.0;
    int m_waveTick = 0;
    int m_blinkTick = 0;
    QTimer m_animTimer;
};
