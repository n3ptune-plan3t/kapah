// Focused window title, elided by the Label's cache. Empty when nothing is
// focused; the title module is the first consumer of the fine-grained
// NiriState signals: only focusedWindowChanged repaints it.

#pragma once

#include "modules.h"

namespace kapah {

class Label;

class WindowTitleItem : public BarModule {
    Q_OBJECT
public:
    WindowTitleItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void refresh();

    Label *m_label = nullptr;
};

} // namespace kapah
