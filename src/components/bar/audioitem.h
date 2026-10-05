// Audio module. Wheel: ±step via nudgeVolume. Left click: toggleMute.
// Right click: sink selector Popup. Sinks are enumerated only while that
// popup is open (roadmap 7.4): enumeration is AudioService::sinks() called in
// onAboutToShow, and the popup is destroyed on dismiss via the grace period.

#pragma once

#include "modules.h"

#include <QTimer>

namespace kapah {

class Label;
class Popup;

class AudioItem : public BarModule {
    Q_OBJECT
public:
    AudioItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void refresh();
    void openSinkSelector();

    Label *m_label = nullptr;
    Popup *m_popup = nullptr;
};

} // namespace kapah
