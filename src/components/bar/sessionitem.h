// Session button: opens the power/session popup (lock, logout, suspend,
// hibernate, reboot, shutdown). Destructive actions within it get a confirm
// step, keyboard-reachable. Session actions use LogindService and niri's Quit
// action; lock spawns kapah-lock (Phase 8 wiring, via SessionSpawner in the
// daemon -- here it falls back to asking logind to lock the session).

#pragma once

#include "modules.h"

namespace kapah {

class IconButton;
class Popup;

class SessionItem : public BarModule {
    Q_OBJECT
public:
    SessionItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

private:
    void openMenu();

    IconButton *m_button = nullptr;
    Popup *m_popup = nullptr;
};

} // namespace kapah
