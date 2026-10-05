#include "popup.h"

#include "icons.h"
#include "theme.h"

#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>

namespace kapah {

Popup::Popup(Theme *theme, IconCache *icons, const QString &nameSpace, QWidget *parent)
    : LayerShellWindow(makeConfig(nameSpace), parent)
    , m_theme(theme)
    , m_icons(icons)
{
    // overlay + exclusive keyboard + no exclusive zone: the compositor hands us
    // the keyboard and no other surface is displaced.
    auto cfg = layerConfig();
    cfg.layer = Layer::Overlay;
    cfg.keyboard = KeyboardInteractivity::Exclusive;
    cfg.exclusiveZone = -1;
    cfg.namespaceString = nameSpace;
    setLayerConfig(cfg);

    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    m_destroyTimer = new QTimer(this);
    m_destroyTimer->setSingleShot(true);
    m_destroyTimer->setTimerType(Qt::VeryCoarseTimer);
    connect(m_destroyTimer, &QTimer::timeout, this, [this] {
        // Only destroy if we are still hidden; a re-open cancels this.
        if (!isVisible()) {
            releaseSurface();
        }
    });

    if (theme != nullptr) {
        connect(theme, &Theme::themeChanged, this, [this] {
            // Theme changed under us: re-apply the surface colours and repaint.
            update();
            requestPaint();
        });
    }
}

Popup::~Popup() = default;

LayerConfig Popup::makeConfig(const QString &nameSpace)
{
    LayerConfig cfg;
    cfg.namespaceString = nameSpace;
    cfg.layer = Layer::Overlay;
    cfg.keyboard = KeyboardInteractivity::Exclusive;
    // -1 == "ignore other exclusive zones": an overlay must never displace the
    // bar or another shell surface.
    cfg.exclusiveZone = -1;
    // Anchored to the top edge of one side; popup() replaces the margins with
    // the computed position.
    cfg.anchors = AnchorFlag::Top | AnchorFlag::Right;
    return cfg;
}

void Popup::setTheme(Theme *theme)
{
    m_theme = theme;
    update();
    requestPaint();
}

void Popup::setDestroyGraceMs(int ms)
{
    m_destroyGraceMs = qMax(0, ms);
}

void Popup::setPlacement(Placement placement)
{
    m_placement = placement;
}

void Popup::setAnchorWidget(const QWidget *anchor)
{
    m_anchor = anchor;
}

QRect Popup::computePlacementRect(QScreen *screen) const
{
    const QRect available = screen != nullptr ? screen->availableGeometry()
                                              : QGuiApplication::primaryScreen()->geometry();

    const QSize hint = sizeHint();
    const QSize minimum = minimumSizeHint();
    QSize content(qMax(minimum.width(), hint.width()), qMax(minimum.height(), hint.height()));
    content = content.boundedTo(available.size() - QSize(8, 8));

    QPoint topLeft = available.topLeft();

    switch (m_placement) {
    case Placement::Centered: {
        topLeft = available.center() - QPoint(content.width() / 2, content.height() / 2);
        break;
    }
    case Placement::BelowAnchor: {
        int anchorBottom = available.top() + 4;
        int anchorLeft = available.left() + 4;
        if (!m_anchor.isNull() && m_anchor->window() != nullptr) {
            anchorBottom = m_anchor->mapToGlobal(QPoint(0, m_anchor->height())).y();
            anchorLeft = m_anchor->mapToGlobal(QPoint(m_anchor->width() / 2, 0)).x()
                - content.width() / 2;
        }
        topLeft = QPoint(anchorLeft, anchorBottom);
        break;
    }
    case Placement::AboveAnchor: {
        int anchorTop = available.top() + 4;
        int anchorLeft = available.left() + 4;
        if (!m_anchor.isNull() && m_anchor->window() != nullptr) {
            anchorTop = m_anchor->mapToGlobal(QPoint(0, 0)).y();
            anchorLeft = m_anchor->mapToGlobal(QPoint(m_anchor->width() / 2, 0)).x()
                - content.width() / 2;
        }
        topLeft = QPoint(anchorLeft, anchorTop - content.height());
        break;
    }
    case Placement::TopRight: {
        topLeft = QPoint(available.right() + 1 - content.width() - (m_theme != nullptr ? m_theme->px(8) : 8),
            available.top() + (m_theme != nullptr ? m_theme->px(8) : 8));
        break;
    }
    case Placement::BottomCenter: {
        topLeft = QPoint(available.center().x() - content.width() / 2,
            available.bottom() + 1 - content.height() - (m_theme != nullptr ? m_theme->px(24) : 24));
        break;
    }
    }

    // Clamp into the available geometry: a popup must never be partially
    // offscreen, because the compositor will not scroll it back.
    topLeft.setX(qBound(available.left(), topLeft.x(), available.right() + 1 - content.width()));
    topLeft.setY(qBound(available.top(), topLeft.y(), available.bottom() + 1 - content.height()));
    return QRect(topLeft, content);
}

void Popup::anchorToScreen(QScreen *screen)
{
    QScreen *target = screen;
    if (target == nullptr && !m_anchor.isNull() && m_anchor->window() != nullptr) {
        target = m_anchor->window()->screen();
    }
    if (target == nullptr) {
        target = QGuiApplication::primaryScreen();
    }

    if (windowHandle() != nullptr && windowHandle()->screen() != target) {
        if (windowHandle()->screen() != nullptr) {
            windowHandle()->screen()->removeWindow(windowHandle());
        }
        target->addWindow(windowHandle());
        if (QWindow *w = windowHandle()) {
            w->setScreen(target);
        }
    }

    const QRect rect = computePlacementRect(target);
    // Layer-shell surfaces position themselves with set_margin, not with a
    // global coordinate. Anchor to the closest edge pair and use margins for the
    // offset, which is how wlr-layer-shell is meant to be used.
    auto cfg = layerConfig();
    const QRect geo = target->geometry();
    const int dx = rect.left() - geo.left();
    const int dy = rect.top() - geo.top();

    const bool nearLeft = dx + rect.width() / 2 < geo.width() / 2;
    cfg.anchors = AnchorFlag::Top;
    cfg.anchors |= nearLeft ? AnchorFlag::Left : AnchorFlag::Right;
    cfg.margins = QMargins(nearLeft ? dx : -(geo.right() + 1 - rect.right()), dy, 0, 0);
    cfg.width = rect.width();
    cfg.height = rect.height();
    cfg.outputName = target->name();
    setLayerConfig(cfg);

    resize(rect.size());
    QWindow *w = windowHandle();
    if (w != nullptr) {
        w->setGeometry(rect.x(), rect.y(), rect.width(), rect.height());
    }
}

void Popup::resizeToContent()
{
    const QSize hint = sizeHint();
    const QSize minimum = minimumSizeHint();
    resize(qMax(minimum.width(), hint.width()), qMax(minimum.height(), hint.height()));
    if (isVisible()) {
        const QRect rect = computePlacementRect(windowHandle() != nullptr ? windowHandle()->screen()
                                                                          : nullptr);
        if (QWindow *w = windowHandle()) {
            w->setGeometry(rect.x(), rect.y(), rect.width(), rect.height());
        }
    }
}

void Popup::popup(QScreen *screen)
{
    if (m_popupOpen) {
        return;
    }
    m_popupOpen = true;
    m_hideIsDismissal = false;

    cancelDestroy();
    onAboutToShow();
    resizeToContent();
    anchorToScreen(screen);
    showForScreen(windowHandle() != nullptr ? windowHandle()->screen() : screen);

    if (QWindow *w = windowHandle()) {
        w->requestActivate();
    }
    setFocus(Qt::OtherFocusReason);

    Q_EMIT popupShown();
}

void Popup::dismiss()
{
    if (!m_popupOpen) {
        return;
    }
    m_popupOpen = false;
    m_hideIsDismissal = true;
    hide();
    Q_EMIT popupDismissed();
}

void Popup::onAboutToShow() {}
void Popup::onHidden() {}

void Popup::scheduleDestroy()
{
    if (m_destroyGraceMs <= 0) {
        releaseSurface();
        return;
    }
    m_destroyTimer->start(m_destroyGraceMs);
}

void Popup::cancelDestroy()
{
    m_destroyTimer->stop();
}

bool Popup::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate) {
        // The compositor revoked our keyboard (another surface took focus). That
        // means the user clicked somewhere else, which is a dismissal.
        if (m_popupOpen) {
            Q_EMIT dismissRequested();
        }
        return true;
    }
    return LayerShellWindow::event(event);
}

void Popup::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT dismissRequested();
        event->accept();
        return;
    }
    LayerShellWindow::keyPressEvent(event);
}

void Popup::focusOutEvent(QFocusEvent *event)
{
    // Only treat a focus loss as a dismissal if the whole window deactivated;
    // moving focus between widgets inside the popup fires this too.
    if (!isActiveWindow()) {
        Q_EMIT dismissRequested();
    }
    LayerShellWindow::focusOutEvent(event);
}

void Popup::showEvent(QShowEvent *event)
{
    LayerShellWindow::showEvent(event);
    Q_UNUSED(event)
}

void Popup::hideEvent(QHideEvent *event)
{
    LayerShellWindow::hideEvent(event);
    onHidden();
    if (m_popupOpen) {
        m_popupOpen = false;
        Q_EMIT popupDismissed();
    }
    // A dismissal must destroy the surface immediately: the whole point of the
    // grace period is to make a *re-open* cheap, and a dismissed popup is not
    // coming back.
    if (m_hideIsDismissal) {
        releaseSurface();
    } else {
        scheduleDestroy();
    }
}

} // namespace kapah