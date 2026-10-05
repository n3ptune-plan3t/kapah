// Backlight module. Wheel adjusts via setPercent(); the service writes via
// logind (or sysfs as a fallback) and reports brightnessChangedByUser so the
// OSD can fire once per gesture.

#pragma once

#include "modules.h"

namespace kapah {

class Label;

class BacklightItem : public BarModule {
    Q_OBJECT
public:
    BacklightItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;

private:
    void refresh();

    Label *m_label = nullptr;
};

} // namespace kapah
