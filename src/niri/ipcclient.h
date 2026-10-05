// niri IPC transport.
//
// Wire protocol, transcribed from niri-ipc 26.4.0 and verified against
// niri/src/ipc/server.rs:
//
//   * A Unix SOCK_STREAM socket named by $NIRI_SOCKET.
//   * Newline-delimited JSON in both directions.
//   * Every request gets exactly one reply, in order.
//   * "EventStream" is special: after its `Handled` reply, niri stops reading
//     requests on that socket and writes an unbounded event stream to it.
//
// That last point is why this class owns *two* sockets. niri's own docs are
// explicit: "If you'd like to read an event stream and write more requests at
// the same time, you need to use two IPC sockets." The event socket here is
// read-only after the handshake; the request socket is write-and-read.
//
// Everything is event driven: one QSocketNotifier per socket, enabled only when
// the socket is connected, and a single-shot QTimer for reconnect backoff. There
// is no polling anywhere in this file.

#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>

#include <functional>

class QSocketNotifier;
class QTimer;

namespace kapah::niri {

// One connection to niri, either the event stream or a request channel.
class Connection : public QObject {
    Q_OBJECT
public:
    Connection(const QString &name, QObject *parent = nullptr);
    ~Connection() override;

    // Socket path. Empty means "read $NIRI_SOCKET on open()".
    void setSocketPath(const QString &path) { m_socketPath = path; }
    QString socketPath() const { return m_socketPath; }
    void setName(const QString &name) { m_name = name; }
    QString name() const { return m_name; }

    bool isConnected() const { return m_fd < 0; }

    // Connects. Returns false immediately on failure; onSuccess/onFailure are
    // called later from the event loop.
    void open();
    void close();

    void sendLine(const QByteArray &line);
    void send(RequestKind kind, const QJsonObject &payload = {});

Q_SIGNALS:
    // A complete newline-delimited JSON object arrived.
    void lineReceived(const QByteArray &line);
    void connected();
    void disconnected(const QString &reason);

private:
    void handleReadable();
    void setNotifierEnabled(bool enabled);

    QString m_name;
    QString m_socketPath;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_in;
};

// Retrying wrapper around Connection, with exponential backoff capped at 5 s.
class RetryingConnection : public QObject {
    Q_OBJECT
public:
    RetryingConnection(const QString &name, QObject *parent = nullptr);
    ~RetryingConnection() override;

    void setSocketPath(const QString &path);
    QString socketPath() const;

    // Seconds to wait after the n-th consecutive failure: min(0.25 * 2^n, 5).
    static int backoffMs(int attempt);

    // Stops retrying. Used when niri is not running at all and we do not want to
    // spin forever (the shell still starts, it just has no compositor state).
    void stop();

    bool everConnected() const { return m_everConnected; }
    int attemptCount() const { return m_attempt; }

    Connection *connection() { return m_connection; }

Q_SIGNALS:
    void lineReceived(const QByteArray &line);
    void connected();
    void disconnected(const QString &reason);

private:
    void scheduleReconnect();
    void attemptConnect();

    Connection *m_connection = nullptr;
    QTimer *m_retryTimer = nullptr;
    QString m_socketPath;
    int m_attempt = 0;
    bool m_everConnected = false;
    bool m_stopped = false;
};

// ---------------------------------------------------------------------------
// IpcClient
// ---------------------------------------------------------------------------

// High-level facade. Owns the two sockets, applies the reconnect policy, and
// hands out a fire-and-forget request API plus a callback API for the handful of
// requests that need a reply.
class IpcClient : public QObject {
    Q_OBJECT
public:
    using ReplyHandler = std::function<void(const Reply &)>;

    explicit IpcClient(QObject *parent = nullptr);
    ~IpcClient() override;

    // Resolves $NIRI_SOCKET. Safe to call before the event loop starts.
    void start();

    bool eventStreamActive() const { return m_events->everConnected(); }
    bool requestChannelActive() const { return m_requests->everConnected(); }

    // Raw fire-and-forget. Errors are logged, not reported.
    void send(RequestKind kind, const QJsonObject &payload = {});

    // With a reply handler. The handler runs on the main thread.
    void sendWithReply(RequestKind kind, const QJsonObject &payload, ReplyHandler handler);

    // Convenience for the hot paths.
    void action(const QJsonObject &actionJson);
    void actionWithReply(const QJsonObject &actionJson, ReplyHandler handler);

Q_SIGNALS:
    // A decoded event from the stream. This is the compositor's only push
    // channel; everything else is a request/reply.
    void eventReceived(const kapah::niri::Event &event);

    // The event stream connected for the first time. NiriState waits for this
    // before it declares itself "ready".
    void eventStreamConnected();
    void eventStreamDisconnected(const QString &reason);
    // Version string from Request::Version, once.
    void versionReceived(const QString &rawVersion);
    void requestFailed(const QString &message);

private:
    void handleEventLine(const QByteArray &line);
    void pumpReplies();

    RetryingConnection *m_events = nullptr;
    RetryingConnection *m_requests = nullptr;
    QHash<quint64, ReplyHandler> m_pending;
    quint64 m_nextRequestId = 1;
    bool m_versionRequested = false;
};

} // namespace kapah::niri