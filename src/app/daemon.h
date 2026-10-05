// The composition root. One process, owned in one place, with a defined
// construction order (config -> globals -> plugin -> niri -> services -> UI).
// Destruction is the reverse, via member declaration order plus explicit
// stop() calls where a service is fd-owned.
//
// Model B (ADR-001): this process never owns the session lock. For that it
// spawns kapah-lock, taking a logind delay inhibitor while kapah-lock comes
// up so suspend cannot race ahead of the lock.

#pragma once

#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QTimer>

#include "config.h"
#include "logind.h"

#include <optional>

namespace kapah {

class ActionSender;
class AudioService;
class BacklightService;
class Bar;
class ConfigWatcher;
class Globals;
class IpcClient;
class IdleService;
class IpcServer;
class LogindService;
class NetworkService;
class NiriState;
class PowerService;
class SessionLock;
class Theme;
class IconCache;
class TrayService;

namespace niri {
class ActionSender;
class IpcClient;
class NiriState;
}

class Daemon : public QObject {
    Q_OBJECT
public:
    explicit Daemon(QObject *parent = nullptr);
    ~Daemon() override;

    void setConfigFilePath(const QString &path);
    void setPerfLogging(bool enabled);

    // Runs the construction order and starts every service. Returns false
    // only when the shell cannot be in a usable state at all (no display).
    bool start();

    // `kapahctl status` reads these.
    Theme *theme() const { return m_theme; }
    niri::NiriState *niriState() const { return m_state; }
    ConfigWatcher *configWatcher() const { return m_watcher; }

Q_SIGNALS:
    void ready();

private:
    void spawnLock();
    void onLockProcessExited(int exitCode, QProcess::ExitStatus status);
    void onAboutToSuspend();

    QString m_configFilePath;

    ConfigWatcher *m_watcher = nullptr;
    Globals *m_globals = nullptr;
    niri::IpcClient *m_ipc = nullptr;
    niri::NiriState *m_state = nullptr;
    niri::ActionSender *m_actions = nullptr;

    PowerService *m_power = nullptr;
    NetworkService *m_network = nullptr;
    AudioService *m_audio = nullptr;
    BacklightService *m_backlight = nullptr;
    TrayService *m_tray = nullptr;
    LogindService *m_logind = nullptr;

    Theme *m_theme = nullptr;
    IconCache *m_icons = nullptr;
    Bar *m_bar = nullptr;

    IdleService *m_idle = nullptr;
    IpcServer *m_ipcServer = nullptr;

    QProcess *m_lockProcess = nullptr;
    QTimer m_lockInhibitorTimer;
    // The delay inhibitor held between PrepareForSleep and the lock being
    // up. Held as a member so its lifetime spans the suspend delay window.
    std::optional<LogindService::Inhibitor> m_sleepInhibitor;
};

} // namespace kapah
