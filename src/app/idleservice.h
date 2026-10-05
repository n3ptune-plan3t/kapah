// Idle handling for kapahd: translates "no input for N seconds" into the
// configured action cascade (dim, lock, DPMS off, suspend). One
// IdleNotifier per step, each with its own timeout -- one notification
// object per (timeout, seat), which is what the v1/v2 protocol supports.
//
// Every cascade resets on `resumed()`: the first sign of user input
// disarms every pending effect (a later step that already fired, e.g. dim,
// is undone when possible: dim is restored to the pre-dim level).
//
// Inhibitors from the [idle] config live as LogindService::Inhibitor members,
// so their lifetime is the scope's lifetime (roadmap Phase 8.3).

#pragma once

#include <QObject>

#include <optional>
#include <vector>

#include "logind.h"

class QTimer;

namespace kapah {

class BacklightService;
class ConfigWatcher;
class IdleNotifier;

namespace niri {
class ActionSender;
}

class IdleService : public QObject {
    Q_OBJECT
public:
    explicit IdleService(ConfigWatcher *watcher, QObject *parent = nullptr);
    ~IdleService() override;

    void setServices(LogindService *logind, niri::ActionSender *actions, struct wl_seat *seat,
        BacklightService *backlight = nullptr);
    void start();
    void stop();

    bool enabled() const { return m_enabled; }

private:
    void rearm();
    void onLockTimeout();
    void onDimTimeout();
    void onDpmsOffTimeout();
    void onSuspendTimeout();
    void restoreBrightness();

    ConfigWatcher *m_watcher = nullptr;
    LogindService *m_logind = nullptr;
    BacklightService *m_backlight = nullptr;
    niri::ActionSender *m_actions = nullptr;
    struct wl_seat *m_seat = nullptr;

    IdleNotifier *m_dimNotifier = nullptr;
    IdleNotifier *m_lockNotifier = nullptr;
    IdleNotifier *m_dpmsNotifier = nullptr;
    IdleNotifier *m_suspendNotifier = nullptr;

    std::vector<LogindService::Inhibitor> m_inhibitors;
    int m_savedBrightness = -1;
    bool m_enabled = false;
    bool m_dimmed = false;
    bool m_dimmed = false;
};

} // namespace kapah
