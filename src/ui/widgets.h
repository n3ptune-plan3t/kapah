// Primitive widgets. Hand-painted, no QSS (roadmap 4.2).
//
// Every one of these:
//   * takes a Theme* and an IconCache* in its constructor rather than reaching
//     for singletons, so the gallery and the tests can drive them;
//   * paints only in paintEvent, calling update() and never repaint();
//   * sets Qt::WA_OpaquePaintEvent when it fully covers its rect, so the
//     compositor does not blend a background we are about to overwrite;
//   * has no timers of its own.

#pragma once

#include <QAbstractButton>
#include <QLabel>
#include <QWidget>

#include <functional>

class QPainter;
class QPaintEvent;

namespace kapah {

class IconCache;
class Theme;

// ---------------------------------------------------------------------------
// Label -- elided, layout-cached text
// ---------------------------------------------------------------------------

// QLabel is not used because it caches a QPixmap-based layout that we invalidate
// on every resize, and because we need sub-pixel elision against a themed font.
// This is a 60-line replacement that does exactly what the bar needs.
class Label : public QWidget {
    Q_OBJECT
public:
    explicit Label(Theme *theme, QWidget *parent = nullptr);
    ~Label() override;

    void setText(const QString &text);
    QString text() const { return m_text; }

    void setRole(int role); // 0 small, 1 regular, 2 medium, 3 bold, 4 mono
    void setColor(const QString &colorName);
    void setAlignment(Qt::Alignment align) { m_align = align; }
    void setElideMode(Qt::TextElideMode mode) { m_elideMode = mode; }
    // Renders the text in a dimmed colour when false, for "no data yet" states.
    void setDimmed(bool dimmed);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void recalcCache();
    void invalidateCache();

    Theme *m_theme = nullptr;
    QString m_text;
    QString m_colorName = QLatin1String(palette::Foreground);
    int m_role = 1;
    Qt::Alignment m_align = Qt::AlignLeft | Qt::AlignVCenter;
    Qt::TextElideMode m_elideMode = Qt::ElideRight;
    bool m_dimmed = false;

    // Elision cache. Keyed on (text, width, colorName, dimmed). A bar repaint
    // after a window title change elides once, then every subsequent frame is a
    // memcpy.
    QString m_cachedElided;
    int m_cachedWidth = -1;
    QString m_cachedColor;
    bool m_cacheValid = false;
};

// ---------------------------------------------------------------------------
// IconButton -- a button whose whole content is an icon (or a glyph)
// ---------------------------------------------------------------------------

class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    enum class Shape {
        Plain,    // transparent, no hover fill (bar modules)
        Pill,     // hover fill with the large radius (toolbar buttons)
        Circle,   // hover fill, fully round (session button)
    };
    Q_ENUM(Shape)

    explicit IconButton(Theme *theme, IconCache *icons, QWidget *parent = nullptr);

    void setIconName(const QString &name);
    void setColorName(const QString &colorName);
    void setShape(Shape shape);
    void setIconSize(int logicalSize);
    void setPadding(int logicalPadding);
    // A short label next to the icon (the power menu's buttons).
    void setText(const QString &text);
    // Draws attention without stealing focus. Used for the low-battery case.
    void setAlert(bool alert);
    bool alert() const { return m_alert; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    Theme *m_theme = nullptr;
    IconCache *m_icons = nullptr;
    QString m_iconName;
    QString m_text;
    QString m_colorName = QLatin1String(palette::Foreground);
    Shape m_shape = Shape::Plain;
    int m_iconSize = 18;
    int m_padding = 4;
    bool m_alert = false;
    bool m_hovered = false;
    bool m_down = false;
};

// ---------------------------------------------------------------------------
// Pill -- a small stateful chip: workspace indicators, DND, profile names
// ---------------------------------------------------------------------------

class Pill : public QWidget {
    Q_OBJECT
public:
    enum class State {
        Normal,
        Active,
        Urgent,
        Disabled,
    };
    Q_ENUM(State)

    explicit Pill(Theme *theme, QWidget *parent = nullptr);

    void setText(const QString &text);
    void setState(State state);
    void setSelected(bool selected);
    void setCompact(bool compact);
    // Draws an underline in `color` instead of a background change. Workspace
    // indicators use this so the bar does not turn into a wall of pills.
    void setIndicatorOnly(bool indicatorOnly);
    void setIndicatorColor(const QString &colorName);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Theme *m_theme = nullptr;
    QString m_text;
    State m_state = State::Normal;
    bool m_selected = false;
    bool m_compact = false;
    bool m_indicatorOnly = false;
    QString m_indicatorColor = QLatin1String(palette::Accent);
    mutable QSize m_cachedHint;
    mutable bool m_hintValid = false;
};

// ---------------------------------------------------------------------------
// Slider -- volume and brightness
// ---------------------------------------------------------------------------

// A horizontal track with a filled portion and a handle. Wheel and drag both
// work; keyboard arrows work; the value is clamped to [0,1] and rounded to
// `step()` so that "volume +5" is exactly reproducible.
class Slider : public QWidget {
    Q_OBJECT
public:
    Slider(Theme *theme, QWidget *parent = nullptr);

    void setValue(double value);
    double value() const { return m_value; }
    void setStep(int step) { m_step = qMax(1, step); }
    int step() const { return m_step; }
    // 0 means unlimited. Used by the brightness slider on displays with a max
    // below 100.
    void setMaximumPercent(int percent) { m_maxPercent = qBound(0, percent, 100); }
    int maximumPercent() const { return m_maxPercent; }

    void setLabel(const QString &label);
    void setIconName(const QString &iconName);
    void setShowValue(bool show) { m_showValue = show; }
    void setColorName(const QString &colorName);

    QSize sizeHint() const override;

Q_SIGNALS:
    void valueChanged(double value);
    // Emitted at the end of a drag, for OSD suppression while scrubbing.
    void valueCommitted(double value);
    void activated();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setValueFromX(int x);
    void emitChanged();

    Theme *m_theme = nullptr;
    double m_value = 0.0;
    int m_step = 5;
    int m_maxPercent = 100;
    bool m_dragging = false;
    bool m_hovered = false;
    bool m_showValue = false;
    QString m_label;
    QString m_iconName;
    QString m_colorName = QLatin1String(palette::Accent);
};

// ---------------------------------------------------------------------------
// Toggle -- a switch
// ---------------------------------------------------------------------------

class Toggle : public QAbstractButton {
    Q_OBJECT
public:
    explicit Toggle(Theme *theme, QWidget *parent = nullptr);

    void setText(const QString &text);
    void setDescription(const QString &description);
    void setIconName(const QString &iconName);
    // Disabled reads as "on but not available", e.g. bluetooth present but off
    // because no adapter exists.
    void setUnsupported(bool unsupported);
    bool unsupported() const { return m_unsupported; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    Theme *m_theme = nullptr;
    IconCache *m_icons = nullptr;
    QString m_text;
    QString m_description;
    QString m_iconName;
    bool m_hovered = false;
    bool m_unsupported = false;
};

// ---------------------------------------------------------------------------
// Separator
// ---------------------------------------------------------------------------

class Separator : public QWidget {
    Q_OBJECT
public:
    enum class Orientation { Horizontal, Vertical };
    Q_ENUM(Orientation)
    explicit Separator(Theme *theme, QOrientation orientation, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Theme *m_theme = nullptr;
    QOrientation m_orientation;
};

// ---------------------------------------------------------------------------
// ScrollFrame -- puts a thin overlay scrollbar on a clipped child
// ---------------------------------------------------------------------------
//
// The launcher's result list and the notification centre both need a scrollbar.
// Qt's QScrollArea brings in a QScrollBar style and a viewport widget; for a
// hand-painted virtualized list that is pure overhead. This is a clip + a painted
// 5 px overlay thumb, and it repaints only when the offset changes.

class ScrollFrame : public QWidget {
    Q_OBJECT
public:
    explicit ScrollFrame(Theme *theme, QWidget *parent = nullptr);

    void setContentHeight(int height);
    int contentHeight() const { return m_contentHeight; }
    void setOffset(int offset);
    int offset() const { return m_offset; }
    int viewportHeight() const { return m_viewportHeight; }
    int maxOffset() const { return qMax(0, m_contentHeight - m_viewportHeight); }

    // Called when the wheel or a drag moved the offset. The child uses it to
    // decide which rows to paint.
    void setOnOffsetChanged(std::function<void(int)> cb) { m_onOffsetChanged = std::move(cb); }
    void setOnScrolledToEnd(std::function<void()> cb) { m_onScrolledToEnd = std::move(cb); }

Q_SIGNALS:
    void offsetChanged(int offset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void clampOffset();

    Theme *m_theme = nullptr;
    int m_contentHeight = 0;
    int m_offset = 0;
    int m_viewportHeight = 0;
    bool m_dragging = false;
    int m_dragAnchorOffset = 0;
    int m_dragAnchorY = 0;
    bool m_interactionHappened = false;
    std::function<void(int)> m_onOffsetChanged;
    std::function<void()> m_onScrolledToEnd;
};

} // namespace kapah