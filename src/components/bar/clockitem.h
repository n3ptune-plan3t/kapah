// Clock module: minute-aligned single-shot QTimer with Qt::VeryCoarseTimer
// (roadmap 4.1). No repeating QTimer. Shows HH:mm; clicking opens the Calendar
// in a Popup whose surface is destroyed on dismiss after the grace period.

#pragma once

#include "modules.h"

#include <QTimer>

namespace kapah {

class Calendar;
class Label;
class Popup;

class ClockItem : public BarModule {
    Q_OBJECT
public:
    ClockItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);
    ~ClockItem() override;

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void tick();
    void armTimer();
    void refreshText();

    Label *m_label = nullptr;
    QTimer m_timer;
    Popup *m_popup = nullptr;
    Calendar *m_calendar = nullptr;
};

} // namespace kapah
