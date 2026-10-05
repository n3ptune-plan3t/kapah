#include "idleservice.h"

#include "actionsender.h"
#include "backlight.h"
#include "config.h"
#include "idlenotify.h"
#include "logind.h"
#include "logging.h"

#include <QCoreApplication>
#include <QProcess>

#include <wayland-client-protocol.h>

namespace kapah {

IdleService::IdleService(ConfigWatcher *watcher, QObject *parent)
    : QObject(parent)
    , m_watcher(watcher)
{
}

IdleService::~IdleService()
{
    stop();
}

void IdleService::setServices(LogindService *logind, niri::ActionSender *actions, wl_seat *seat,
    BacklightService *backlight)
{
    m_logind = logind;
    m_actions = actions;
    m_seat = seat;
    m_backlight = backlight;
}

void IdleService::start()
{
    const IdleConfig &cfg = m_watcher->config().idle;

    m_dimNotifier = new IdleNotifier(this);
    m_dimNotifier->setTimeoutMs(qMax(0, cfg.dimSeconds) * 1000u);
    m_dimNotifier->setSeat(m_seat);
    connect(m_dimNotifier, &IdleNotifier::idled, this, &IdleService::onDimTimeout);
    connect(m_dimNotifier, &IdleNotifier::resumed, this, [this] { restoreBrightness(); });

    m_lockNotifier = new IdleNotifier(this);
    m_lockNotifier->setTimeoutMs(qMax(0, cfg.lockSeconds) * 1000u);
    m_lockNotifier->setSeat(m_seat);
    connect(m_lockNotifier, &IdleNotifier::idled, this, &IdleService::onLockTimeout);
    connect(m_lockNotifier, &IdleNotifier::resumed, this, [] {});

    m_dpmsNotifier = new IdleNotifier(this);
    m_dpmsNotifier->setTimeoutMs(qMax(0, cfg.dpmsOffSeconds) * 1000u);
    m_dpmsNotifier->setSeat(m_seat);
    connect(m_dpmsNotifier, &IdleNotifier::idled, this, &IdleService::onDpmsOffTimeout);
    connect(m_dpmsNotifier, &IdleNotifier::resumed, this,
        [this] { if (m_actions != nullptr) m_actions->powerOnMonitors(); });

    if (cfg.suspendSeconds > 0) {
        m_suspendNotifier = new IdleNotifier(this);
        m_suspendNotifier->setTimeoutMs(static_cast<quint32>(cfg.suspendSeconds) * 1000u);
        m_suspendNotifier->setSeat(m_seat);
        connect(m_suspendNotifier, &IdleNotifier::idled, this,
            &IdleService::onSuspendTimeout);
    }

    // One notifier per timeout. Ignore "available" of each: if the
    // compositor lacks the protocol we silently run without idle detection,
    // matching fail-soft (roadmap 10.7).
    for (IdleNotifier *n :
        { m_dimNotifier, m_lockNotifier, m_dpmsNotifier, m_suspendNotifier }) {
        if (n != nullptr) {
            n->start();
        }
    }

    m_enabled = true;
}

void IdleService::stop()
{
    m_enabled = false;
    for (IdleNotifier *n :
        { m_dimNotifier, m_lockNotifier, m_dpmsNotifier, m_suspendNotifier }) {
        if (n != nullptr) {
            n->stop();
            n->deleteLater();
        }
    }
    m_dimNotifier = nullptr;
    m_lockNotifier = nullptr;
    m_dpmsNotifier = nullptr;
    m_suspendNotifier = nullptr;
}

void IdleService::onLockTimeout()
{
    // Spawn kapah-lock. On timeout we could also call logind's LockSession,
    // which returns fast and is idempotent, but the design calls for the
    // explicit lock binary.
    if (m_actions != nullptr) {
        // Ensure monitors are on so the user sees they are locked.
        m_actions->powerOnMonitors();
    }
    QProcess::startDetached(QStringLiteral("kapah-lock"), QStringList());
}

void IdleService::onDimTimeout()
{
    if (m_backlight != nullptr && m_backlight->available()) {
        m_savedBrightness = m_backlight->percent();
        const int dimPercent = m_watcher->config().idle.dimPercent;
        m_backlight->setPercent(dimPercent);
        m_dimmed = true;
    }
}

void IdleService::onDpmsOffTimeout()
{
    if (m_actions != nullptr) {
        m_actions->powerOffMonitors();
    }
}

void IdleService::onSuspendTimeout()
{
    if (m_logind != nullptr) {
        m_logind->suspend();
    }
}

void IdleService::restoreBrightness()
{
    if (m_dimmed && m_savedBrightness >= 0 && m_backlight != nullptr) {
        m_backlight->setPercent(m_savedBrightness);
        m_dimmed = false;
    }
}

} // namespace kapah
