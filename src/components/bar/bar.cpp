#include "bar.h"

#include "config.h"
#include "globals.h"
#include "logging.h"
#include "state.h"
#include "theme.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QScreen>
#include <QWidget>

namespace kapah {

namespace {

// Fullscreen heuristic: niri does not expose a fullscreen flag on windows,
// but a fullscreen tile looks exactly like a geometry that covers the
// output. A window counts as fullscreen for auto-hide purposes when it is
// the active window of its workspace, is not floating, and its tile matches
// the output size within a 2-px tolerance. Documented in HANDOVER §6 step 2;
// when niri adds an explicit fullscreen field to niri-ipc, switch to it.
bool windowIsFullscreen(const niri::WindowInfo &w, const niri::WorkspaceInfo &ws,
    QScreen *screen)
{
    if (w.isFloating || !ws.isActive) {
        return false;
    }
    const QSize screenSize = screen->size();
    return qAbs(w.layout.tileWidth - screenSize.width()) <= 2.0
        && qAbs(w.layout.tileHeight - screenSize.height()) <= 2.0
        && w.layout.viewX <= 0.5 && w.layout.viewY <= 0.5;
}

bool outputHasFullscreenWindow(const niri::NiriState *state, QScreen *screen)
{
    if (state == nullptr || screen == nullptr) {
        return false;
    }
    const QString outputName = screen->name();
    for (const niri::WorkspaceInfo &ws : state->workspacesForOutput(outputName)) {
        if (!ws.isActive) {
            continue;
        }
        for (const niri::WindowInfo &w : state->windows()) {
            if (w.hasWorkspace && w.workspaceId == ws.id
                && windowIsFullscreen(w, ws, screen)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

BarItem::BarItem(const BarContext &ctx, QScreen *screen, const BarConfig &cfg, QWidget *parent)
    : LayerShellWindow(LayerConfig{}, parent)
{
    m_ctx = ctx;
    m_screen = screen;
    m_config = cfg;
    // The initial surface config: full-width strip at the configured edge.
    {
        LayerConfig lc;
        const bool top = cfg.position == BarPosition::Top;
        lc.anchors = AnchorFlag::Left | AnchorFlag::Right
            | (top ? AnchorFlag::Top : AnchorFlag::Bottom);
        lc.height = cfg.height;
        lc.width = 0; // compositor stretches us across the output
        lc.exclusiveZone = cfg.height;
        lc.margins = cfg.margins;
        lc.layer = Layer::Top;
        lc.keyboard = KeyboardInteractivity::None;
        setLayerConfig(lc);
    }

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(ctx.theme->px(4));

    m_left = new QWidget(this);
    m_center = new QWidget(this);
    m_right = new QWidget(this);
    new QHBoxLayout(m_left)->setContentsMargins(0, 0, 0, 0);
    new QHBoxLayout(m_center)->setContentsMargins(0, 0, 0, 0);
    new QHBoxLayout(m_right)->setContentsMargins(0, 0, 0, 0);

    row->addWidget(m_left, 0);
    row->addWidget(m_center, 1);
    row->addWidget(m_right, 0);

    rebuildFromConfig(cfg);
}

void BarItem::rebuildFromConfig(const BarConfig &cfg)
{
    m_config = cfg;

    // Replace the layer config: position/anchors/exclusive zone from the new
    // config. Anchors derive from Position (top/bottom), the bar spans the
    // full width, and reserves its height.
    LayerConfig lc = layerConfig();
    const bool top = cfg.position == BarPosition::Top;
    lc.anchors = AnchorFlag::Left | AnchorFlag::Right | (top ? AnchorFlag::Top : AnchorFlag::Bottom);
    lc.height = cfg.height;
    lc.width = 0; // full width: zero means "let the compositor stretch".
    lc.exclusiveZone = cfg.height;
    lc.margins = cfg.margins;
    lc.layer = Layer::Top;
    lc.keyboard = KeyboardInteractivity::None;
    setLayerConfig(lc);

    // Rebuild the three sections.
    auto fill = [this](QWidget *section, const BarSectionConfig &sectionCfg) {
        auto *lay = qobject_cast<QHBoxLayout *>(section->layout());
        while (QLayoutItem *item = lay->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (BarModuleId id : sectionCfg.modules) {
            if (BarModule *module = makeModule(id, m_ctx, m_screen, section)) {
                lay->addWidget(module);
            }
        }
    };
    fill(m_left, cfg.left);
    fill(m_center, cfg.center);
    fill(m_right, cfg.right);

    if (m_ctx.theme != nullptr) {
        // Theme metrics changed: resize ourselves and re-place the surface.
        setFixedHeight(cfg.height);
    }
    showForScreen(m_screen);
}

Bar::Bar(const BarContext &ctx, ConfigWatcher *watcher, QObject *parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_watcher(watcher)
{
    watcher->onBarChanged([this] { rebuildAll(); });
}

Bar::~Bar()
{
    for (BarItem *item : m_items) {
        item->releaseSurface();
        item->deleteLater();
    }
    m_items.clear();
}

void Bar::start()
{
    syncWithScreens();
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &Bar::syncWithScreens);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &Bar::syncWithScreens);
    if (m_ctx.state != nullptr) {
        connect(m_ctx.state, &niri::NiriState::outputsChanged, this, &Bar::syncWithScreens);
        connect(m_ctx.state, &niri::NiriState::windowsChanged, this, &Bar::rebuildAll);
        connect(m_ctx.state, &niri::NiriState::workspaceActivatedChanged, this, &Bar::rebuildAll);
    }
}

void Bar::syncWithScreens()
{
    // Remove items whose screen is gone.
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (!QGuiApplication::screens().contains(m_items.at(i)->screen())) {
            m_items.at(i)->releaseSurface();
            m_items.at(i)->deleteLater();
            m_items.removeAt(i);
        }
    }
    // Add new screens.
    for (QScreen *screen : QGuiApplication::screens()) {
        bool have = false;
        for (BarItem *item : m_items) {
            if (item->screen() == screen) {
                have = true;
                break;
            }
        }
        if (!have) {
            const BarConfig cfg = m_watcher != nullptr
                ? m_watcher->config().bar.forOutput(screen->name())
                : BarConfig{};
            auto *item = new BarItem(m_ctx, screen, cfg, nullptr);
            m_items.append(item);
            applyVisibility(item, cfg);
        }
    }
}

void Bar::rebuildAll()
{
    for (BarItem *item : m_items) {
        const BarConfig cfg = m_watcher != nullptr
            ? m_watcher->config().bar.forOutput(item->screen()->name())
            : BarConfig{};
        item->rebuildFromConfig(cfg);
        applyVisibility(item, cfg);
    }
    syncWithScreens();
}

void Bar::applyVisibility(BarItem *item, const BarConfig &cfg)
{
    switch (cfg.visibility) {
    case BarVisibility::Always:
        item->show();
        break;
    case BarVisibility::Hidden:
        item->hide();
        break;
    case BarVisibility::AutoHide:
        if (outputHasFullscreenWindow(m_ctx.state, item->screen())) {
            item->hide();
        } else {
            item->show();
        }
        break;
    }
}

} // namespace kapah
