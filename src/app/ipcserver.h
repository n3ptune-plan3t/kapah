// kapahd's control socket: roadmap section 8. Newline-delimited JSON at
// $XDG_RUNTIME_DIR/kapah.sock (paths::controlSocket()). The wire is a tiny
// JSON protocol: one request object per line, one response object per line.
//
// Version guard: kapahctl refuses to talk to a shelld whose
// KAPAH_IPC_VERSION_MAJOR differs, which is why the constants live next to
// the version header.
//
// Commands that target components which do not exist yet (launcher, OSD,
// notification daemon, quick settings, power menu) return
// {"ok":false,"error":"not implemented"} rather than pretending to work.

#pragma once

#include <QLocalServer>
#include <QObject>

namespace kapah {

class ActionSender;
class AudioService;
class BacklightService;
class ConfigWatcher;
class LogindService;
class NetworkService;
class NiriState;
class PowerService;
class TrayService;

namespace niri {
class ActionSender;
class NiriState;
}

class IpcServer : public QLocalServer {
    Q_OBJECT
public:
    IpcServer(ConfigWatcher *watcher, niri::NiriState *state, PowerService *power,
        NetworkService *network, AudioService *audio, BacklightService *backlight,
        TrayService *tray, LogindService *logind, niri::ActionSender *actions,
        QObject *parent = nullptr);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    QByteArray dispatch(const QByteArray &line);
    QByteArray statusJson() const;

    ConfigWatcher *m_watcher = nullptr;
    niri::NiriState *m_state = nullptr;
    PowerService *m_power = nullptr;
    NetworkService *m_network = nullptr;
    AudioService *m_audio = nullptr;
    BacklightService *m_backlight = nullptr;
    TrayService *m_tray = nullptr;
    LogindService *m_logind = nullptr;
    niri::ActionSender *m_actions = nullptr;
};

} // namespace kapah
