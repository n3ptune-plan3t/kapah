// Tray module: one row of tray item icons, normally capped. Left click
// activates (async via TrayService::activate); right click opens the context
// menu if the item has one. Tray icons honour the same render-once IconCache
// strategy.

#pragma once

#include "modules.h"

namespace kapah {

class TrayItemView : public BarModule {
    Q_OBJECT
public:
    TrayItemView(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void refresh();
};

} // namespace kapah
