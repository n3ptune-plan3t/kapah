#include "ipcserver.h"

#include "actionsender.h"
#include "audio.h"
#include "backlight.h"
#include "config.h"
#include "logind.h"
#include "logging.h"
#include "network.h"
#include "paths.h"
#include "power.h"
#include "state.h"
#include "tray.h"

#include "kapah/version.h"

#include <QCoreApplication>
#include <QFile>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostInfo>
#include <QTimer>

namespace kapah {

IpcServer::IpcServer(ConfigWatcher *watcher, niri::NiriState *state, PowerService *power,
    NetworkService *network, AudioService *audio, BacklightService *backlight,
    TrayService *tray, LogindService *logind, niri::ActionSender *actions,
    QObject *parent)
    : QLocalServer(parent)
    , m_watcher(watcher)
    , m_state(state)
    , m_power(power)
    , m_network(network)
    , m_audio(audio)
    , m_backlight(backlight)
    , m_tray(tray)
    , m_logind(logind)
    , m_actions(actions)
{
    const QString path = paths::controlSocket();
    // Deliberately no D-Bus mirror at launch: one protocol, one parser.
    if (!listen(path)) {
        // A stale socket file from a crash leaves the address busy; unlink
        // and retry once. That is not a security hole because the runtime
        // dir is already per-user private (0700).
        QFile::remove(path);
        if (!listen(path)) {
            // If that still fails the shell keeps running; kapahctl prints a
            // clear error. Emits error via QLocalServer::errorOccurred.
            LOG_WARN(Ipc, "cannot listen on the control socket");
        }
    }
}

void IpcServer::incomingConnection(qintptr socketDescriptor)
{
    auto *socket = new QLocalSocket(this);
    socket->setSocketDescriptor(socketDescriptor);
    QObject::connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        while (socket->canReadLine()) {
            const QByteArray line = socket->readLine().trimmed();
            socket->write(dispatch(line));
        }
    });
    QObject::connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
}

namespace {

QByteArray okJson()
{
    return QByteArrayLiteral("{\"ok\":true}\n");
}

QByteArray errJson(const QString &msg)
{
    QJsonObject o;
    o[QStringLiteral("ok")] = false;
    o[QStringLiteral("error")] = msg;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

QByteArray IpcServer::dispatch(const QByteArray &line)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return errJson(QStringLiteral("invalid json"));
    }
    const QJsonObject req = doc.object();
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    if (cmd.isEmpty()) {
        return errJson(QStringLiteral("missing cmd"));
    }

    // Version guard first, exactly once.
    const int clientMajor = req.value(QStringLiteral("major")).toInt(-1);
    if (clientMajor >= 0 && clientMajor != KAPAH_IPC_VERSION_MAJOR) {
        return errJson(QStringLiteral("version mismatch"));
    }

    const QJsonArray args = req.value(QStringLiteral("args")).toArray();
    const auto argAt = [args](int i) -> QString {
        return (i < args.size()) ? args.at(i).toString() : QString();
    };

    if (cmd == QLatin1String("reload")) {
        m_watcher->reloadNow();
        return okJson();
    }
    if (cmd == QLatin1String("quit")) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return okJson();
    }
    if (cmd == QLatin1String("status")) {
        return statusJson();
    }
    if (cmd == QLatin1String("perf")) {
        return errJson(QStringLiteral("not implemented"));
    }
    if (cmd == QLatin1String("volume")) {
        const QString sub = argAt(0);
        if (m_audio == nullptr || !m_audio->available()) {
            return errJson(QStringLiteral("audio unavailable"));
        }
        if (sub == QLatin1String("up")) {
            m_audio->nudgeVolume(argAt(1).isEmpty() ? m_audio->step() : argAt(1).toInt());
            return okJson();
        }
        if (sub == QLatin1String("down")) {
            m_audio->nudgeVolume(-(argAt(1).isEmpty() ? m_audio->step() : argAt(1).toInt()));
            return okJson();
        }
        if (sub == QLatin1String("mute")) {
            m_audio->toggleMute();
            return okJson();
        }
        return errJson(QStringLiteral("unknown volume subcommand"));
    }
    if (cmd == QLatin1String("brightness")) {
        const QString sub = argAt(0);
        if (m_backlight == nullptr || !m_backlight->available()) {
            return errJson(QStringLiteral("backlight unavailable"));
        }
        const int step = argAt(1).isEmpty() ? m_backlight->step() : argAt(1).toInt();
        if (sub == QLatin1String("up")) {
            m_backlight->setPercent(m_backlight->percent() + step);
            return okJson();
        }
        if (sub == QLatin1String("down")) {
            m_backlight->setPercent(m_backlight->percent() - step);
            return okJson();
        }
        return errJson(QStringLiteral("unknown brightness subcommand"));
    }
    if (cmd == QLatin1String("lock")
        || cmd == QLatin1String("launcher") || cmd == QLatin1String("osd")
        || cmd == QLatin1String("notify") || cmd == QLatin1String("powermenu")
        || cmd == QLatin1String("quicksettings")) {
        return errJson(QStringLiteral("not implemented"));
    }
    return errJson(QStringLiteral("unknown command"));
}

QByteArray IpcServer::statusJson() const
{
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    o[QStringLiteral("audio")] = m_audio != nullptr && m_audio->available();
    o[QStringLiteral("network")] = m_network != nullptr && m_network->available();
    o[QStringLiteral("power")] = m_power != nullptr && m_power->available();
    o[QStringLiteral("backlight")] = m_backlight != nullptr && m_backlight->available();
    o[QStringLiteral("tray")] = m_tray != nullptr && m_tray->available();
    o[QStringLiteral("logind")] = m_logind != nullptr && m_logind->available();
    o[QStringLiteral("niri")] = m_state != nullptr && m_state->isReady();
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace kapah
