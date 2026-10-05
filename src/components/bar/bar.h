// The bar: one BarItem (layer-shell surface) per output, driven entirely by
// Config sections and fine-grained NiriState signals. Visibility per output
// from BarConfig::forOutput: Always, Hidden, or AutoHide (hide while a
// fullscreen window covers the output -- see the heuristic documented in
// bar.cpp).
//
// The bar is created in the daemon (Step 3) with every service pointer it
// needs; nothing here does service discovery. When a service daemon is
// killed, the service emits availableChanged(false) and the widgets go into
// their "unavailable" state without rebuilding.

#pragma once

#include <QObject>
#include <QVector>

#include "layersurface.h"
#include "modules.h"

class QScreen;

namespace kapah {

class ConfigWatcher;

class BarItem : public LayerShellWindow {
    Q_OBJECT
public:
    BarItem(const BarContext &ctx, QScreen *screen, const BarConfig &cfg, QWidget *parent = nullptr);

    // Tears the section contents down and rebuilds from the (possibly new)
    // config. Called from ConfigWatcher::onBarChanged.
    void rebuildFromConfig(const BarConfig &cfg);

    QScreen *screen() const { return m_screen; }

private:
    BarConfig m_config;
    BarContext m_ctx;
    QScreen *m_screen = nullptr;
    QWidget *m_left = nullptr;
    QWidget *m_center = nullptr;
    QWidget *m_right = nullptr;
};

class Bar : public QObject {
    Q_OBJECT
public:
    Bar(const BarContext &ctx, ConfigWatcher *watcher, QObject *parent = nullptr);
    ~Bar() override;

    void start();
    // Release every surface (zero cost at idle when the session locks).
    void releaseAll();

    int surfaceCount() const { return static_cast<int>(m_items.size()); }

private:
    void syncWithScreens();
    void rebuildAll();
    void applyVisibility(BarItem *item, const BarConfig &cfg);

    BarContext m_ctx;
    ConfigWatcher *m_watcher = nullptr;
    QVector<BarItem *> m_items;
};

} // namespace kapah
