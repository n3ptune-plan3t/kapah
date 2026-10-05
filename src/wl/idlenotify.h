// ext-idle-notify-v1 client.
//
// Roadmap Phase 8.3: "implement ext-idle-notify-v1 client in shelld (or reuse
// niri's/swayidle-compatible tooling, but own implementation is preferred for
// fewer processes)".
//
// One IdleNotifier owns one `ext_idle_notification_v1`. The protocol has two
// notification requests: `get_idle_notification` (v1) and
// `get_input_idle_notification` (v2). We use the v2 input one when available and
// fall back to v1, because "input idle" is the signal a user expects to mean
// "nobody has touched the machine".
//
// No timers here: the idled/resumed events arrive from the compositor. The timers
// that *consume* idleness (dim/lock/DPMS/suspend) live in IdleService and are
// single-shot, re-armed on every resumed event.

#pragma once

#include <QObject>
#include <QString>

struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;

namespace kapah {

class IdleNotifier : public QObject {
    Q_OBJECT
public:
    explicit IdleNotifier(QObject *parent = nullptr);
    ~IdleNotifier() override;

    // Binds ext_idle_notifier_v1. Returns false when the compositor does not
    // advertise it, in which case available() stays false and IdleService stays
    // disabled. Capability detection, not an assumption (roadmap 6).
    bool start();
    void stop();

    // One notification per instance, so the service creates one IdleNotifier
    // per enabled step (dim, lock, dpoff, suspend), each with its own
    // timeout. The seat must be known before start(); the idle request
    // references it.
    void setTimeoutMs(quint32 ms) { m_timeoutMs = ms; }
    void setSeat(wl_seat *seat) { m_seat = seat; }

    bool available() const { return m_available; }
    // True when we got the v2 `get_input_idle_notification` variant.
    bool inputIdle() const { return m_inputIdle; }
    quint32 protocolVersion() const { return m_version; }

    // Mirrors what we told the notification object; reported by `kapahctl status`.
    bool isIdle() const { return m_idle; }

    // Called from the wayland listener. Public only because the listener is a
    // free function; do not call these directly.
    void onIdled();
    void onResumed();

Q_SIGNALS:
    void idled();
    void resumed();
    // A protocol failure. Fatal for idling only; the rest of the shell is fine.
    void failed(const QString &reason);

private:
    ext_idle_notifier_v1 *m_notifier = nullptr;
    ext_idle_notification_v1 *m_notification = nullptr;
    bool m_available = false;
    bool m_inputIdle = false;
    bool m_idle = false;
    quint32 m_version = 0;
    quint32 m_timeoutMs = 0;
    struct wl_seat *m_seat = nullptr;
};

} // namespace kapah