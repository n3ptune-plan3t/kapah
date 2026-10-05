// The contract every system service implements (roadmap 4.4).
//
//   "Every service (NetworkService, PowerService, ...) exposes: state() snapshot,
//    changed() signal, available() boolean. UI must render a sane 'unavailable'
//    state, never crash if a daemon is missing."
//
// Three rules follow from that and are enforced by the base class:
//
//  1. No service ever blocks the main thread. D-Bus calls are pending calls with
//     a watcher; syscalls are non-blocking or short; anything genuinely blocking
//     is documented in the header with the reason.
//
//  2. No service ever polls. A service that cannot be event-driven says so in its
//     header and explains the fallback (see PowerService, which has a udev path
//     precisely so it does not have to).
//
//  3. Every service survives its daemon disappearing. On `lost()` the service
//     emits available()==false, clears its snapshot to a defined "unavailable"
//     state rather than stale data, and waits for `found()`.

#pragma once

#include <QObject>
#include <QTimer>

namespace kapah {

// Mixin that gives a service its retry schedule. A single-shot QTimer, re-armed
// only after a failure, with exponential backoff capped at 5 s. This is the only
// permitted periodic-ish construct outside the clock, and it exists solely to
// recover from a daemon restart.
class RetryPolicy {
public:
    explicit RetryPolicy(QObject *owner, int baseMs = 500, int maxMs = 5000);

    void arm();
    void cancel();
    bool pending() const { return m_pending; }
    int attempt() const { return m_attempt; }

    void setOnRetry(std::function<void()> cb) { m_onRetry = std::move(cb); }

    static int backoffMs(int attempt, int baseMs, int maxMs);

private:
    QObject *m_owner = nullptr;
    QTimer *m_timer = nullptr;
    std::function<void()> m_onRetry;
    int m_baseMs = 500;
    int m_maxMs = 5000;
    int m_attempt = 0;
    bool m_pending = false;
};

// Base for anything that talks to a name-ownable service. Watches the name so
// "the daemon started" is an event rather than a guess.
class ServiceNameWatcher : public QObject {
    Q_OBJECT
public:
    explicit ServiceNameWatcher(const QString &service, const QString &interface, QObject *parent = nullptr);
    ~ServiceNameWatcher() override;

    bool owned() const { return m_owned; }
    QString service() const { return m_service; }
    QString interfaceName() const { return m_interface; }

    // Set false to stop watching entirely (a module that is disabled).
    void setEnabled(bool enabled);

Q_SIGNALS:
    void found();
    void lost();

private:
    void update();
    void setOwned(bool now);

    QString m_service;
    QString m_interface;
    QObject *m_watcher = nullptr;
    RetryPolicy m_retry;
    bool m_owned = false;
    bool m_enabled = true;
};

} // namespace kapah