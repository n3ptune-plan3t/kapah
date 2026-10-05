// Network module. Overview icon derived from NetworkService state; the
// SSID/signal shown in the tooltip. The AP list itself lives in quick
// settings, not the bar.

#pragma once

#include "modules.h"

namespace kapah {

class Label;

class NetworkItem : public BarModule {
    Q_OBJECT
public:
    NetworkItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void refresh();

    Label *m_label = nullptr;
};

} // namespace kapah
