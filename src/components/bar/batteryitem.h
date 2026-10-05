// Battery module. Threshold crossings arrive via
// PowerService::takeLowNotification()/takeCriticalNotification() (once per
// discharge); the bar forwards them once so the notification daemon can alert.
// Until the notification daemon exists (Phase 4) it logs, and renders the
// percent in the urgent colour while in the low zone.

#pragma once

#include "modules.h"

namespace kapah {

class Label;

class BatteryItem : public BarModule {
    Q_OBJECT
public:
    BatteryItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void refresh();

    Label *m_label = nullptr;
    bool m_wasPresent = false;
};

} // namespace kapah
