#include "service.h"

#include "logging.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>

namespace kapah {

// ---------------------------------------------------------------------------
// RetryPolicy
// ---------------------------------------------------------------------------

RetryPolicy::RetryPolicy(QObject *owner, int baseMs, int maxMs)
    : m_owner(owner)
    , m_baseMs(baseMs)
    , m_maxMs(maxMs)
{
    m_timer = new QTimer(owner);
    m_timer->setSingleShot(true);
    // VeryCoarseTimer: a retry after a daemon restart does not care about being
    // a millisecond precise, and coalescing it saves a wakeup.
    m_timer->setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(m_timer, &QTimer::timeout, owner, [this] {
        m_pending = false;
        m_attempt = 0;
        if (m_onRetry) {
            m_onRetry();
        }
    });
}

int RetryPolicy::backoffMs(int attempt, int baseMs, int maxMs)
{
    int ms = baseMs;
    for (int i = 0; i < attempt && ms < maxMs; ++i) {
        ms *= 2;
    }
    return ms > maxMs ? maxMs : ms;
}

void RetryPolicy::arm()
{
    if (m_timer == nullptr) {
        return;
    }
    m_pending = true;
    m_timer->start(backoffMs(m_attempt, m_baseMs, m_maxMs));
    ++m_attempt;
}

void RetryPolicy::cancel()
{
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    m_pending = false;
    m_attempt = 0;
}

// ---------------------------------------------------------------------------
// ServiceNameWatcher
// ---------------------------------------------------------------------------

ServiceNameWatcher::ServiceNameWatcher(
    const QString &service, const QString &interface, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_interface(interface)
    , m_retry(parent, 500, 5000)
{
    m_retry.setOnRetry([this] { update(); });

    m_watcher = new QDBusServiceWatcher(
        m_service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);

    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this,
        [this](const QString &) { setOwned(true); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
        [this](const QString &) { setOwned(false); });

    // One cached check at construction so we know the initial state. This is
    // the only isServiceRegistered() call; afterwards registration state
    // comes from the watcher's owner-change signals, so the retry tick never
    // makes a blocking D-Bus round trip.
    const bool initially = QDBusConnection::sessionBus().interface() != nullptr
        && QDBusConnection::sessionBus().interface()->isServiceRegistered(m_service.toUtf8())
            .value();
    setOwned(initially);
}

ServiceNameWatcher::~ServiceNameWatcher() = default;

void ServiceNameWatcher::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (enabled) {
        update();
    } else {
        m_retry.cancel();
        if (m_owned) {
            m_owned = false;
            Q_EMIT lost();
        }
    }
}

void ServiceNameWatcher::update()
{
    if (!m_enabled) {
        return;
    }
    if (!m_owned) {
        // Still missing: keep a single retry pending so the watcher Revives
        // itself when the daemon appears. The state itself is driven by the
        // QDBusServiceWatcher signals, never by polling.
        m_retry.arm();
    }
}

void ServiceNameWatcher::setOwned(bool now)
{
    if (now == m_owned) {
        if (!now) {
            m_retry.arm();
        }
        return;
    }

    m_owned = now;
    if (now) {
        m_retry.cancel();
        LOG_INFOF(Core, "D-Bus service appeared: %s", qPrintable(m_service));
        Q_EMIT found();
    } else {
        LOG_INFOF(Core, "D-Bus service disappeared: %s", qPrintable(m_service));
        m_retry.arm();
        Q_EMIT lost();
    }
}

} // namespace kapah