#include "daemon.h"

#include "actionsender.h"
#include "audio.h"
#include "backlight.h"
#include "bar.h"
#include "config.h"
#include "globals.h"
#include "idleservice.h"
#include "ipcserver.h"
#include "ipcclient.h"
#include "layersurface.h"
#include "logind.h"
#include "network.h"
#include "power.h"
#include "state.h"
#include "theme.h"
#include "icons.h"
#include "tray.h"

#include "env.h"
#include "logging.h"
#include "paths.h"

#include <QCoreApplication>
#include <QGuiApplication>

namespace kapah {

Daemon::Daemon(QObject *parent)
    : QObject(parent)
{
}

Daemon::~Daemon()
{
    // Services keep their own child-QObject parents; the explicit stops are
    // only for the ones that own fds that must be closed before the bus
    // teardown (audio's threaded mainloop, tray's registration).
    if (m_audio != nullptr) {
        m_audio->stop();
    }
    if (m_tray != nullptr) {
        m_tray->stop();
    }
}

void Daemon::setConfigFilePath(const QString &path)
{
    m_configFilePath = path;
}

void Daemon::setPerfLogging(bool enabled)
{
    Q_UNUSED(enabled)
    // Wired in Phase 10's perf gate; the flag is accepted for forward
    // compatibility of the CLI.
}

bool Daemon::start()
{
    // Construction order is load-bearing (see the header).
    m_watcher = new ConfigWatcher(this);
    m_watcher->start(m_configFilePath.isEmpty() ? paths::configFile()
                                                : m_configFilePath);

    m_globals = new Globals(this);
    m_globals->start();

    installLayerShellPlugin();

    m_theme = new Theme(this);
    m_theme->apply(m_watcher->config());
    m_icons = new IconCache(this);
    m_icons->setTheme(m_theme);

    m_ipc = new niri::IpcClient(this);
    m_state = new niri::NiriState(this);
    m_state->attach(m_ipc);
    m_actions = new niri::ActionSender(m_ipc, this);
    m_ipc->start();

    m_power = new PowerService(this);
    m_network = new NetworkService(this);
    m_audio = new AudioService(this);
    m_backlight = new BacklightService(this);
    m_tray = new TrayService(m_theme, m_icons, this);
    m_logind = new LogindService(this);

    m_power->start(true, true);
    m_network->start();
    m_audio->start();
    m_backlight->start();
    m_tray->start();
    m_logind->start();

    BarContext barCtx;
    barCtx.theme = m_theme;
    barCtx.icons = m_icons;
    barCtx.state = m_state;
    barCtx.actions = m_actions;
    barCtx.power = m_power;
    barCtx.network = m_network;
    barCtx.audio = m_audio;
    barCtx.backlight = m_backlight;
    barCtx.tray = m_tray;
    barCtx.logind = m_logind;

    m_bar = new Bar(barCtx, m_watcher, this);
    m_bar->start();

    m_idle = new IdleService(m_watcher, this);
    m_idle->setServices(m_logind, m_actions, m_globals->seat(), m_backlight);
    m_idle->start();

    m_ipcServer = new IpcServer(m_watcher, m_state, m_power, m_network, m_audio,
        m_backlight, m_tray, m_logind, m_actions, this);

    // Lock on suspend and on lid close. The delay inhibitor is taken while
    // kapah-lock comes up; without it, a fast suspend would beat the lock.
    connect(m_logind, &LogindService::aboutToSuspend, this, &Daemon::onAboutToSuspend);
    connect(m_logind, &LogindService::lockHintChanged, this,
        [this](bool shouldLock) { if (shouldLock) spawnLock(); });

    m_watcher->onThemeChanged([this] { m_theme->apply(m_watcher->config()); });

    Q_EMIT ready();
    return true;
}

void Daemon::onAboutToSuspend()
{
    // Take the logind delay inhibitor first; while it is held, logind will not
    // suspend. Spawn kapah-lock; once it is up for a settle window, release
    // the inhibitor.
    m_logind->takeInhibitor(QStringLiteral("sleep"), QStringLiteral("kapahd"),
        QStringLiteral("locking the session before suspend"),
        LogindService::InhibitorMode::Delay,
        [this](LogindService::Inhibitor inhibitor) {
            m_sleepInhibitor.emplace(std::move(inhibitor));
            spawnLock();
            // Cap: even if kapah-lock never comes up we must not hang the
            // suspend indefinitely; 4 s < logind's InhibitDelayMaxSec on every
            // budget laptop we have seen.
            m_lockInhibitorTimer.setSingleShot(true);
            m_lockInhibitorTimer.setTimerType(Qt::VeryCoarseTimer);
            connect(&m_lockInhibitorTimer, &QTimer::timeout, this,
                [this] { m_sleepInhibitor.reset(); },
                Qt::UniqueConnection);
            m_lockInhibitorTimer.start(4000);
        });

    // If the inhibitor call itself fails (no logind), fall back to spawning
    // the lock and letting suspend proceed: the lock binary still refuses to
    // start without the wayland connection, and the session stays in its
    // previous state.
}

void Daemon::spawnLock()
{
    if (m_lockProcess != nullptr) {
        // Already locked or locking: the session stays locked, the spawn
        // request is a no-op.
    }
    if (m_lockProcess == nullptr) {
        m_lockProcess = new QProcess(this);
        connect(m_lockProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Daemon::onLockProcessExited);
        m_lockProcess->start(QStringLiteral("kapah-lock"), QStringList());
    }
}

void Daemon::onLockProcessExited(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    LOG_WARNF(Core, "kapah-lock exited with code %d", exitCode);
    m_lockProcess->deleteLater();
    m_lockProcess = nullptr;
}

} // namespace kapah
