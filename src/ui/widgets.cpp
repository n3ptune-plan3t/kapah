#include "widgets.h"

#include "icons.h"
#include "paint.h"
#include "theme.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

namespace kapah {

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

Label::Label(Theme *theme, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

Label::~Label() = default;

void Label::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    invalidateCache();
    updateGeometry();
    update();
}

void Label::setRole(int role)
{
    if (m_role == role) {
        return;
    }
    m_role = role;
    invalidateCache();
    updateGeometry();
    update();
}

void Label::setColor(const QString &colorName)
{
    if (m_colorName == colorName) {
        return;
    }
    m_colorName = colorName;
    invalidateCache();
    update();
}

void Label::setDimmed(bool dimmed)
{
    if (m_dimmed == dimmed) {
        return;
    }
    m_dimmed = dimmed;
    invalidateCache();
    update();
}

void Label::invalidateCache()
{
    m_cacheValid = false;
    m_cachedWidth = -1;
}

void Label::recalcCache()
{
    const QFontMetrics fm(m_theme != nullptr ? m_theme->font(m_role) : QFont());
    const QColor c = m_theme != nullptr ? m_theme->color(m_colorName) : QColor(Qt::black);
    m_cachedElided = elideText(m_text, width(), fm, m_elideMode);
    m_cachedWidth = width();
    m_cachedColor = c.name(QColor::HexArgb);
    m_cacheValid = true;
}

QSize Label::sizeHint() const
{
    const QFontMetrics fm(m_theme != nullptr ? m_theme->font(m_role) : QFont());
    return QSize(fm.horizontalAdvance(m_text), fm.height());
}

QSize Label::minimumSizeHint() const
{
    return QSize(0, m_theme != nullptr ? m_theme->font(m_role).pixelSize() + 2 : 8);
}

void Label::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    if (!m_cacheValid || m_cachedWidth != width()) {
        recalcCache();
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setFont(m_theme->font(m_role));

    QColor c(m_cachedColor);
    if (m_dimmed) {
        c = blend(m_theme->color(QLatin1String(palette::Background)), c, 140);
    }
    p.setPen(c);

    const QFontMetrics fm(m_theme->font(m_role));
    const int baseline = height() / 2 + (fm.ascent() - fm.descent()) / 2;
    p.drawText(QRect(0, 0, width(), height()), m_align | Qt::TextSingleLine, m_cachedElided);
    Q_UNUSED(baseline)
}

void Label::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        invalidateCache();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------
// IconButton
// ---------------------------------------------------------------------------

IconButton::IconButton(Theme *theme, IconCache *icons, QWidget *parent)
    : QAbstractButton(parent)
    , m_theme(theme)
    , m_icons(icons)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::PointingHandCursor);
    // Accessibility (roadmap 10.3): state is never colour-only.
    setAccessibleName(m_text.isEmpty() ? m_iconName : m_text);
}

void IconButton::setIconName(const QString &name)
{
    if (m_iconName == name) {
        return;
    }
    m_iconName = name;
    updateGeometry();
    update();
}

void IconButton::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    setAccessibleName(text.isEmpty() ? m_iconName : text);
    updateGeometry();
    update();
}

void IconButton::setColorName(const QString &colorName)
{
    if (m_colorName == colorName) {
        return;
    }
    m_colorName = colorName;
    update();
}

void IconButton::setShape(Shape shape)
{
    if (m_shape == shape) {
        return;
    }
    m_shape = shape;
    update();
}

void IconButton::setIconSize(int logicalSize)
{
    m_iconSize = logicalSize;
    updateGeometry();
    update();
}

void IconButton::setPadding(int logicalPadding)
{
    m_padding = logicalPadding;
    updateGeometry();
    update();
}

void IconButton::setAlert(bool alert)
{
    if (m_alert == alert) {
        return;
    }
    m_alert = alert;
    setAccessibleDescription(alert ? tr("needs attention") : QString());
    update();
}

QSize IconButton::sizeHint() const
{
    if (m_theme == nullptr) {
        return QSize(m_iconSize + 8, m_iconSize + 8);
    }
    const int icon = m_theme->px(m_iconSize);
    const int pad = m_theme->px(m_padding) * 2;
    if (m_text.isEmpty()) {
        return QSize(icon + pad, icon + pad);
    }
    const QFontMetrics fm(m_theme->smallFont());
    return QSize(icon + fm.horizontalAdvance(m_text) + pad * 2 + m_theme->px(4), icon + pad);
}

void IconButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);

    const Metrics &m = m_theme->metrics();
    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // Background: only when there is something to show. A Plain bar module stays
    // transparent so the bar background is not painted forty times.
    if (m_shape == Shape::Pill || m_shape == Shape::Circle || m_down || m_hovered) {
        const QString bgName = isChecked() || m_down ? QLatin1String(palette::Active)
                                                    : QLatin1String(palette::Hover);
        QColor bg = m_theme->color(bgName);
        if (m_shape == Shape::Circle) {
            const qreal r = qMin(body.width(), body.height()) / 2.0;
            fillRounded(&p, body, r, bg);
        } else {
            fillRounded(&p, body, m_theme->px(m.radius), bg);
        }
    }

    if (hasFocus()) {
        strokeRounded(&p, body.adjusted(1, 1, -1, -1), m_theme->px(m.radius),
            m_theme->color(QLatin1String(palette::Focus)), 1.5);
    }

    QString colorName = m_colorName;
    if (m_alert) {
        colorName = QLatin1String(palette::Urgent);
    } else if (isChecked()) {
        colorName = QLatin1String(palette::Accent);
    }
    const QColor fg = m_theme->color(colorName);

    const int iconPx = m_theme->px(m_iconSize);
    const int pad = m_theme->px(m_padding);
    int x = pad;
    int y = (height() - iconPx) / 2;

    if (m_icons != nullptr && !m_iconName.isEmpty()) {
        const qreal dpr = QPaintDevice::devicePixelRatioF(this);
        const QPixmap pm = m_icons->icon(m_iconName, iconPx, dpr);
        if (!pm.isNull()) {
            // Icons are drawn from the pixmap and then tinted by compositing, so
            // the same cache entry serves every colour.
            const QRect target(x, y, iconPx, iconPx);
            p.drawPixmap(target.topLeft(), pm);
        }
    }

    if (!m_text.isEmpty()) {
        p.setFont(m_theme->smallFont());
        p.setPen(fg);
        const QRect textRect(x + iconPx + m_theme->px(4), 0,
            qMax(0, width() - (x + iconPx + m_theme->px(4)) - pad), height());
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            elideText(m_text, textRect.width(), QFontMetrics(p.font()), Qt::ElideRight));
    }
}

void IconButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void IconButton::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange) {
        update();
    }
    QAbstractButton::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Pill
// ---------------------------------------------------------------------------

Pill::Pill(Theme *theme, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
{
    setAttribute(Qt::WA_Hover, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
}

void Pill::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    m_hintValid = false;
    updateGeometry();
    update();
}

void Pill::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    update();
}

void Pill::setSelected(bool selected)
{
    if (m_selected == selected) {
        return;
    }
    m_selected = selected;
    update();
}

void Pill::setCompact(bool compact)
{
    if (m_compact == compact) {
        return;
    }
    m_compact = compact;
    m_hintValid = false;
    updateGeometry();
    update();
}

void Pill::setIndicatorOnly(bool indicatorOnly)
{
    if (m_indicatorOnly == indicatorOnly) {
        return;
    }
    m_indicatorOnly = indicatorOnly;
    update();
}

void Pill::setIndicatorColor(const QString &colorName)
{
    if (m_indicatorColor == colorName) {
        return;
    }
    m_indicatorColor = colorName;
    update();
}

QSize Pill::sizeHint() const
{
    if (m_hintValid) {
        return m_cachedHint;
    }
    if (m_theme == nullptr) {
        return QSize(16, 16);
    }
    const Metrics &m = m_theme->metrics();
    const int h = m_theme->px(m_compact ? m.rowHeight - 6 : m.rowHeight);
    if (m_text.isEmpty()) {
        m_cachedHint = QSize(h, h);
    } else {
        const QFontMetrics fm(m_theme->smallFont());
        const int padX = m_theme->px(m_compact ? m.spacing : m.spacing * 2);
        m_cachedHint = QSize(fm.horizontalAdvance(m_text) + padX * 2, h);
    }
    m_hintValid = true;
    return m_cachedHint;
}

QSize Pill::minimumSizeHint() const
{
    return sizeHint();
}

void Pill::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);
    const Metrics &m = m_theme->metrics();
    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool active = m_state == State::Active || m_selected;

    if (m_indicatorOnly) {
        // An underline plus the text colour. This is what keeps a bar with eight
        // workspaces from looking like a row of buttons.
        p.setFont(m_theme->smallFont());
        const QColor fg = active ? m_theme->color(QLatin1String(palette::Accent))
                                 : m_theme->color(QLatin1String(palette::Muted));
        p.setPen(m_state == State::Urgent ? m_theme->color(QLatin1String(palette::Urgent)) : fg);
        const QFontMetrics fm(p.font());
        p.drawText(QRect(0, 0, width(), height()), Qt::AlignCenter | Qt::TextSingleLine,
            elideText(m_text, width() - 2, fm, Qt::ElideRight));
        const int underline = qMax(1, m_theme->px(2));
        const QColor u = m_state == State::Urgent
                ? m_theme->color(QLatin1String(palette::Urgent))
                : m_theme->color(m_indicatorColor);
        p.fillRect(QRect(0, height() - underline, width(), underline), u);
        return;
    }

    const QString bgName = active ? QLatin1String(palette::Active)
                                  : QLatin1String(palette::SurfaceAlt);
    QColor bg = m_theme->color(bgName);
    if (m_state == State::Urgent) {
        bg = m_theme->withAlpha(QLatin1String(palette::Urgent), 200);
    } else if (m_state == State::Disabled) {
        bg = m_theme->withAlpha(QLatin1String(palette::Muted), 60);
    }
    fillRounded(&p, body, m_theme->px(m.radius), bg);

    p.setFont(m_theme->smallFont());
    const QColor fg = m_theme->contrastOn(bg);
    p.setPen(m_state == State::Disabled ? m_theme->color(QLatin1String(palette::Muted)) : fg);
    p.drawText(QRect(0, 0, width(), height()), Qt::AlignCenter | Qt::TextSingleLine,
        elideText(m_text, width() - 4, QFontMetrics(p.font()), Qt::ElideRight));
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

Slider::Slider(Theme *theme, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMinimumHeight(theme != nullptr ? theme->px(theme->metrics().rowHeight) : 20);
}

void Slider::setValue(double value)
{
    value = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(m_value, value)) {
        return;
    }
    m_value = value;
    emitChanged();
    update();
}

void Slider::setLabel(const QString &label)
{
    if (m_label == label) {
        return;
    }
    m_label = label;
    updateGeometry();
    update();
}

void Slider::setIconName(const QString &iconName)
{
    if (m_iconName == iconName) {
        return;
    }
    m_iconName = iconName;
    updateGeometry();
    update();
}

void Slider::setColorName(const QString &colorName)
{
    if (m_colorName == colorName) {
        return;
    }
    m_colorName = colorName;
    update();
}

void Slider::emitChanged()
{
    Q_EMIT valueChanged(m_value);
}

QSize Slider::sizeHint() const
{
    if (m_theme == nullptr) {
        return QSize(160, 24);
    }
    const Metrics &m = m_theme->metrics();
    return QSize(m_theme->px(200), m_theme->px(m.rowHeight));
}

void Slider::setValueFromX(int x)
{
    // Track geometry, matching paintEvent.
    int left = 0;
    if (!m_label.isEmpty() || !m_iconName.isEmpty()) {
        left = m_theme != nullptr ? m_theme->px(m_theme->metrics().iconSmall) + m_theme->px(6) : 24;
    }
    int right = width();
    if (m_showValue) {
        right -= (m_theme != nullptr ? m_theme->px(34) : 34);
    }
    const int track = qMax(1, right - left);
    const double raw = static_cast<double>(x - left) / track;
    const double snapped = m_step > 0 ? std::round(raw * (100.0 / m_step)) * (m_step / 100.0) : raw;
    setValue(snapped);
}

void Slider::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);
    const Metrics &m = m_theme->metrics();

    int left = 0;
    if (!m_label.isEmpty() || !m_iconName.isEmpty()) {
        left = m_theme->px(m.iconSmall) + m_theme->px(6);
    }
    int right = width();
    if (m_showValue) {
        right -= m_theme->px(34);
    }

    // Label / icon.
    if (!m_label.isEmpty() || !m_iconName.isEmpty()) {
        const int iconPx = m_theme->px(m.iconSmall);
        p.setFont(m_theme->smallFont());
        p.setPen(m_theme->color(QLatin1String(palette::Muted)));
        const QRect iconRect(0, (height() - iconPx) / 2, iconPx, iconPx);
        p.drawText(QRect(iconPx + m_theme->px(4), 0, left - m_theme->px(6), height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, m_label);
        Q_UNUSED(iconRect)
    }

    const QRectF track(left, height() / 2.0 - m_theme->px(2) / 2.0,
        qMax(1, right - left), m_theme->px(4));
    const double level = m_maxPercent > 0 ? qMin(1.0, m_value * 100.0 / m_maxPercent) : m_value;
    drawLevelBar(&p, track, m_theme->color(QLatin1String(palette::SurfaceAlt)),
        m_theme->color(m_colorName), level);

    // Handle: a circle at the filled end. Only drawn when hovered or focused, so
    // an idle bar does not paint ten extra shapes.
    if (m_hovered || hasFocus() || m_dragging) {
        const qreal r = m_theme->px(6);
        const qreal cx = track.left() + track.width() * level;
        const QColor handle = m_theme->color(QLatin1String(palette::Foreground));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(hasFocus() ? m_theme->color(QLatin1String(palette::Focus)) : handle);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, track.center().y()), r, r);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    if (m_showValue) {
        p.setFont(m_theme->smallFont());
        p.setPen(m_theme->color(QLatin1String(palette::Muted)));
        p.drawText(QRect(right + m_theme->px(4), 0, width() - right - m_theme->px(4), height()),
            Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine,
            QStringLiteral("%1%").arg(qRound(m_value * 100)));
    }
}

void Slider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_interactionHappened = true;
    setValueFromX(static_cast<int>(event->position().x()));
    Q_EMIT activated();
}

void Slider::mouseMoveEvent(QMouseEvent *event)
{
    m_hovered = true;
    if (m_dragging) {
        setValueFromX(static_cast<int>(event->position().x()));
    } else {
        update();
    }
}

void Slider::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        setValueFromX(static_cast<int>(event->position().x()));
        Q_EMIT valueCommitted(m_value);
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void Slider::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    // One wheel notch is one step, matching the volume key. Ignore horizontal
    // wheels entirely so a horizontal scroll gesture does not change volume.
    const int direction = delta > 0 ? 1 : -1;
    setValue(m_value + direction * (m_step / 100.0));
    Q_EMIT valueCommitted(m_value);
    event->accept();
}

void Slider::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        setValue(m_value - (m_step / 100.0));
        Q_EMIT valueCommitted(m_value);
        event->accept();
        return;
    case Qt::Key_Right:
    case Qt::Key_Up:
        setValue(m_value + (m_step / 100.0));
        Q_EMIT valueCommitted(m_value);
        event->accept();
        return;
    case Qt::Key_Home:
        setValue(0.0);
        Q_EMIT valueCommitted(m_value);
        event->accept();
        return;
    case Qt::Key_End:
        setValue(1.0);
        Q_EMIT valueCommitted(m_value);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// Toggle
// ---------------------------------------------------------------------------

Toggle::Toggle(Theme *theme, QWidget *parent)
    : QAbstractButton(parent)
    , m_theme(theme)
    , m_icons(nullptr)
{
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::PointingHandCursor);
}

void Toggle::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    setAccessibleName(text);
    updateGeometry();
    update();
}

void Toggle::setDescription(const QString &description)
{
    if (m_description == description) {
        return;
    }
    m_description = description;
    setAccessibleDescription(description);
    updateGeometry();
    update();
}

void Toggle::setIconName(const QString &iconName)
{
    if (m_iconName == iconName) {
        return;
    }
    m_iconName = iconName;
    updateGeometry();
    update();
}

void Toggle::setUnsupported(bool unsupported)
{
    if (m_unsupported == unsupported) {
        return;
    }
    m_unsupported = unsupported;
    setEnabled(!unsupported);
    update();
}

QSize Toggle::sizeHint() const
{
    if (m_theme == nullptr) {
        return QSize(160, 28);
    }
    const Metrics &m = m_theme->metrics();
    const QFontMetrics fm(m_theme->smallFont());
    const int h = qMax(m_theme->px(m.rowHeight), fm.height() * (m_description.isEmpty() ? 1 : 2));
    int w = m_theme->px(m.iconLarge) + m_theme->px(8) + fm.horizontalAdvance(m_text) + m_theme->px(48);
    if (!m_description.isEmpty()) {
        w = qMax(w, fm.horizontalAdvance(m_description) + m_theme->px(56));
    }
    return QSize(w, h);
}

void Toggle::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);
    const Metrics &m = m_theme->metrics();

    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_hovered) {
        fillRounded(&p, body, m_theme->px(m.radius),
            m_theme->withAlpha(QLatin1String(palette::Hover), 200));
    }

    if (!m_iconName.isEmpty() && m_icons != nullptr) {
        const int iconPx = m_theme->px(m.iconLarge);
        const QPixmap pm = m_icons->icon(m_iconName, iconPx, devicePixelRatioF());
        if (!pm.isNull()) {
            p.drawPixmap(QPoint(m_theme->px(m.spacing), (height() - iconPx) / 2), pm);
        }
    }

    const int textLeft = m_iconName.isEmpty() ? m_theme->px(m.spacing) : m_theme->px(m.iconLarge) + m_theme->px(8);
    const int switchW = m_theme->px(m.rowHeight);
    const int switchH = m_theme->px(m.rowHeight - 8);

    p.setFont(m_theme->smallFont());
    const QFontMetrics fm(p.font());
    const int textW = qMax(0, width() - textLeft - switchW - m_theme->px(10));

    if (m_description.isEmpty()) {
        p.setPen(m_theme->color(QLatin1String(palette::Foreground)));
        p.drawText(QRect(textLeft, 0, textW, height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            elideText(m_text, textW, fm, Qt::ElideRight));
    } else {
        p.setPen(m_theme->color(QLatin1String(palette::Foreground)));
        p.drawText(QRect(textLeft, height() / 2 - fm.height(), textW, fm.height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            elideText(m_text, textW, fm, Qt::ElideRight));
        p.setPen(m_theme->color(QLatin1String(palette::Muted)));
        const QFont small = m_theme->smallFont();
        small.setPointSizeF(qMax(6.0, small.pointSizeF() - 1.0));
        p.setFont(small);
        const QFontMetrics sfm(small);
        p.drawText(QRect(textLeft, height() / 2, textW, sfm.height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            elideText(m_description, textW, sfm, Qt::ElideRight));
    }

    // The switch itself. A rounded track plus a knob; the knob is drawn at the
    // left when off and the right when on, and a hatched fill plus reduced
    // opacity mark the unsupported state so it is not colour-only.
    const QRectF track(width() - switchW - m_theme->px(m.spacing), height() / 2.0 - switchH / 2.0,
        switchW, switchH);
    const bool on = isChecked() && !m_unsupported;
    const QColor trackColor = on ? m_theme->color(QLatin1String(palette::Accent))
                                 : m_theme->color(QLatin1String(palette::SurfaceAlt));
    fillRounded(&p, track, switchH / 2.0, trackColor);

    const qreal knobR = switchH / 2.0 - m_theme->px(2);
    const qreal knobX = on ? track.right() - knobR : track.left() + knobR;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(m_unsupported ? m_theme->color(QLatin1String(palette::Muted))
                             : m_theme->color(QLatin1String(palette::Foreground)));
    p.drawEllipse(QPointF(knobX, track.center().y()), knobR, knobR);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (hasFocus()) {
        strokeRounded(&p, body.adjusted(1, 1, -1, -1), m_theme->px(m.radius),
            m_theme->color(QLatin1String(palette::Focus)), 1.5);
    }
}

void Toggle::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void Toggle::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void Toggle::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange) {
        update();
    }
    QAbstractButton::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Separator
// ---------------------------------------------------------------------------

Separator::Separator(Theme *theme, QOrientation orientation, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
    , m_orientation(orientation)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    if (orientation == Qt::Horizontal) {
        setFixedHeight(1);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setFixedWidth(1);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
}

QSize Separator::sizeHint() const
{
    return m_orientation == Qt::Horizontal ? QSize(1, 1) : QSize(1, 1);
}

void Separator::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);
    separatorLine(&p, rect(), m_orientation == Qt::Horizontal,
        m_theme->withAlpha(QLatin1String(palette::Border), 180));
}

// ---------------------------------------------------------------------------
// ScrollFrame
// ---------------------------------------------------------------------------

ScrollFrame::ScrollFrame(Theme *theme, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    // A virtualized list paints itself; ScrollFrame only owns the offset and the
    // overlay thumb, so it must not clip paintEvent of its own child.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::StrongFocus);
}

void ScrollFrame::setContentHeight(int height)
{
    if (m_contentHeight == height) {
        return;
    }
    m_contentHeight = qMax(0, height);
    clampOffset();
    update();
}

void ScrollFrame::setOffset(int offset)
{
    const int clamped = qBound(0, offset, maxOffset());
    if (clamped == m_offset) {
        return;
    }
    m_offset = clamped;
    if (m_onOffsetChanged) {
        m_onOffsetChanged(m_offset);
    }
    Q_EMIT offsetChanged(m_offset);
    update();
}

void ScrollFrame::clampOffset()
{
    if (m_offset > maxOffset()) {
        m_offset = maxOffset();
    }
    if (m_offset < 0) {
        m_offset = 0;
    }
}

void ScrollFrame::resizeEvent(QResizeEvent *event)
{
    m_viewportHeight = event->size().height();
    clampOffset();
    QWidget::resizeEvent(event);
}

void ScrollFrame::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    // Three lines per notch, matching Qt's own default and what a trackpad
    // flick expects.
    const int rows = 3;
    const int rowHeight = m_theme != nullptr ? m_theme->px(m_theme->metrics().rowHeight) : 28;
    const int pixels = delta > 0 ? rows : -rows;
    m_interactionHappened = true;
    setOffset(m_offset + pixels * rowHeight / 3);
    // Scrolling to the very bottom is how the user asks for more results in the
    // launcher, so the callback is the notification.
    if (m_offset == maxOffset() && m_onScrolledToEnd) {
        m_onScrolledToEnd();
    }
    event->accept();
}

void ScrollFrame::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || maxOffset() <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragAnchorY = static_cast<int>(event->position().y());
    m_dragAnchorOffset = m_offset;
    event->accept();
}

void ScrollFrame::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        return;
    }
    const int dy = static_cast<int>(event->position().y()) - m_dragAnchorY;
    // Rubber-band resistance above and below the ends, which is the only
    // "animation" in the shell and it is a constant, not a tween.
    int target = m_dragAnchorOffset - dy;
    if (target < 0) {
        target = target / 3;
    } else if (target > maxOffset()) {
        target = maxOffset() + (target - maxOffset()) / 3;
    }
    setOffset(target);
    event->accept();
}

void ScrollFrame::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        setOffset(m_offset); // clamps back
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ScrollFrame::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr || m_viewportHeight <= 0) {
        return;
    }
    if (maxOffset() <= 0) {
        return;
    }
    QPainter p(this);
    const int barW = m_theme->px(m_theme->metrics().scrollbarWidth);
    const double fraction = qBound(0.05, static_cast<double>(m_viewportHeight) / m_contentHeight, 1.0);
    const int thumbH = qMax(m_theme->px(24), static_cast<int>(m_viewportHeight * fraction));
    const double range = maxOffset();
    const double pos = range > 0 ? static_cast<double>(m_offset) / range : 0.0;
    const int thumbY = static_cast<int>(pos * (m_viewportHeight - thumbH));

    fillRounded(&p,
        QRectF(width() - barW - 1, thumbY, barW, thumbH), barW / 2.0,
        m_theme->withAlpha(QLatin1String(palette::Muted), 160));
}

} // namespace kapah