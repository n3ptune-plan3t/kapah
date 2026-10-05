// Layout indicator: current XKB layout name from the niri event stream.
// Never reads evdev, never polls (the whole point of niri's
// KeyboardLayoutSwitched event).

#pragma once

#include "modules.h"

namespace kapah {

class Label;

class LayoutItem : public BarModule {
    Q_OBJECT
public:
    LayoutItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void refresh();

    Label *m_label = nullptr;
};

} // namespace kapah
