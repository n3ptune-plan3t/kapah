#include "ipcclient.h"

#include "logging.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace kapah::niri {

namespace {

// One JSON object per line. The longest legal line is a WindowsChanged event
// with a few hundred windows, so 4 MiB is a generous ceiling that still bounds
// a malicious or broken compositor from eating the heap.
constexpr int kMaxLineBytes = 4 * 1024 * 1024;

bool writeAll(int fd, const char *data, qint64 len)
{
    qint64 written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, static_cast<size_t>(len - written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The socket buffer is full. This should never happen for our
                // request volume (a few hundred bytes per action), but if it
                // does, retry rather than truncating the JSON.
                struct timespec ts = { 0, 1'000'000 }; // 1 ms
                nanosleep(&ts, nullptr);
                continue;
            }
            return false;
        }
        written += n;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(const QString &name, QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_fd(-1)
{
}

Connection::~Connection()
{
    close();
}

void Connection::setNotifierEnabled(bool enabled)
{
    if (m_notifier != nullptr) {
        if (enabled && !m_notifier->isEnabled()) {
            m_notifier->setEnabled(true);
        } else if (!enabled && m_notifier->isEnabled()) {
            m_notifier->setEnabled(false);
        }
    }
}

void Connection::open()
{
    if (m_fd >= 0) {
        return;
    }

    QString path = m_socketPath;
    if (path.isEmpty()) {
        path = QString::fromLocal8Bit(qgetenv("NIRI_SOCKET"));
    }
    if (path.isEmpty()) {
        Q_EMIT disconnected(QStringLiteral("NIRI_SOCKET is not set"));
        return;
    }

    if (path.size() >= static_cast<int>(sizeof(sockaddr_un::sun_path))) {
        Q_EMIT disconnected(QStringLiteral("NIRI_SOCKET path is too long"));
        return;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        Q_EMIT disconnected(QStringLiteral("socket() failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    ::memcpy(addr.sun_path, path.toUtf8().constData(),
        static_cast<size_t>(path.toUtf8().size()));

    const int rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (rc != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            const QString why = QString::fromLocal8Bit(strerror(errno));
            ::close(fd);
            Q_EMIT disconnected(QStringLiteral("connect(%1) failed: %2").arg(path, why));
            return;
        }
        // EINPROGRESS: for a Unix socket the connection is effectively already
        // established, but check with poll(0) so we do not report a success that
        // is really ECONNREFUSED.
        struct pollfd pfd = { fd, POLLOUT, 0 };
        const int pollRc = ::poll(&pfd, 1, 0);
        if (pollRc == 0) {
            ::close(fd);
            Q_EMIT disconnected(QStringLiteral("connect(%1) timed out").arg(path));
            return;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            ::close(fd);
            Q_EMIT disconnected(
                QStringLiteral("connect(%1) failed: %2").arg(path, QString::fromLocal8Bit(strerror(soerr))));
            return;
        }
    }

    // A shell socket should never inherit an over-large receive buffer.
    int rcvbuf = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    m_fd = fd;
    m_in.clear();

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Connection::handleReadable);

    LOG_INFOF(Niri, "%s: connected to %s", qPrintable(m_name), qPrintable(path));
    Q_EMIT connected();
}

void Connection::close()
{
    if (m_notifier != nullptr) {
        m_notifier->setEnabled(false);
        delete m_notifier;
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_in.clear();
}

void Connection::sendLine(const QByteArray &line)
{
    if (m_fd < 0) {
        LOG_WARNF(Niri, "%s: dropping a request, not connected", qPrintable(m_name));
        return;
    }
    QByteArray payload = line;
    if (payload.isEmpty() || payload.back() != '\n') {
        payload.append('\n');
    }
    if (!writeAll(m_fd, payload.constData(), payload.size())) {
        const QString why = QString::fromLocal8Bit(strerror(errno));
        close();
        Q_EMIT disconnected(QStringLiteral("write failed: %1").arg(why));
    }
}

void Connection::send(RequestKind kind, const QJsonObject &payload)
{
    const QByteArray line = encodeRequest(kind, payload);
    if (line.isEmpty()) {
        LOG_WARNF(Niri, "refusing to send an unknown request kind %d", static_cast<int>(kind));
        return;
    }
    sendLine(line);
}

void Connection::handleReadable()
{
    if (m_fd < 0) {
        return;
    }

    char buffer[16384];
    bool sawEof = false;

    while (true) {
        const ssize_t n = ::read(m_fd, buffer, sizeof(buffer));
        if (n > 0) {
            m_in.append(buffer, static_cast<int>(n));
            if (m_in.size() > kMaxLineBytes && m_in.indexOf('\n') < 0) {
                LOG_ERRF(Niri, "%s: dropping a %d byte line with no newline",
                    qPrintable(m_name), m_in.size());
                close();
                Q_EMIT disconnected(QStringLiteral("oversized line"));
                return;
            }
            // Keep draining: the notifier is level-triggered, so returning with
            // data still buffered would cost another epoll wakeup per chunk.
            continue;
        }
        if (n == 0) {
            sawEof = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        {
            const QString why = QString::fromLocal8Bit(strerror(errno));
            close();
            Q_EMIT disconnected(QStringLiteral("read failed: %1").arg(why));
            return;
        }
    }

    // Emit complete lines.
    int start = 0;
    while (true) {
        const int nl = m_in.indexOf('\n', start);
        if (nl < 0) {
            break;
        }
        QByteArray line = m_in.mid(start, nl - start);
        start = nl + 1;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        Q_EMIT lineReceived(line);
    }
    if (start > 0) {
        m_in.remove(0, start);
    }

    if (sawEof) {
        // Anything left in m_in is a truncated line; there is no way to complete
        // it, so drop it.
        close();
        Q_EMIT disconnected(QStringLiteral("niri closed the connection"));
    }
}

// ---------------------------------------------------------------------------
// RetryingConnection
// ---------------------------------------------------------------------------

RetryingConnection::RetryingConnection(const QString &name, QObject *parent)
    : QObject(parent)
    , m_connection(new Connection(name, this))
    , m_retryTimer(new QTimer(this))
    , m_socketPath(QString::fromLocal8Bit(qgetenv("NIRI_SOCKET")))
{
    m_retryTimer->setSingleShot(true);
    // VeryCoarseTimer so a reconnect burst coalesces with other wakeups.
    m_retryTimer->setTimerType(Qt::VeryCoarseTimer);

    connect(m_connection, &Connection::lineReceived, this, &RetryingConnection::lineReceived);
    connect(m_connection, &Connection::connected, this, [this] {
        m_attempt = 0;
        m_everConnected = true;
        m_retryTimer->stop();
        Q_EMIT connected();
    });
    connect(m_connection, &Connection::disconnected, this, [this](const QString &reason) {
        Q_EMIT disconnected(reason);
        scheduleReconnect();
    });
    connect(m_retryTimer, &QTimer::timeout, this, &RetryingConnection::attemptConnect);
}

RetryingConnection::~RetryingConnection() = default;

void RetryingConnection::setSocketPath(const QString &path)
{
    m_socketPath = path;
    m_connection->setSocketPath(path);
}

QString RetryingConnection::socketPath() const
{
    return m_socketPath;
}

int RetryingConnection::backoffMs(int attempt)
{
    // 250 ms, 500 ms, 1 s, 2 s, 4 s, 5 s, 5 s, ...
    int ms = 250;
    for (int i = 0; i < attempt && ms < 5000; ++i) {
        ms *= 2;
    }
    return ms > 5000 ? 5000 : ms;
}

void RetryingConnection::stop()
{
    m_stopped = true;
    m_retryTimer->stop();
    m_connection->close();
}

void RetryingConnection::scheduleReconnect()
{
    if (m_stopped) {
        return;
    }
    ++m_attempt;
    const int ms = backoffMs(m_attempt);
    LOG_INFOF(Niri, "%s: reconnecting in %d ms (attempt %d)",
        qPrintable(m_connection->name()), ms, m_attempt);
    m_retryTimer->start(ms);
}

void RetryingConnection::attemptConnect()
{
    if (m_stopped) {
        return;
    }
    m_connection->open();
}

// ---------------------------------------------------------------------------
// IpcClient
// ---------------------------------------------------------------------------

IpcClient::IpcClient(QObject *parent)
    : QObject(parent)
    , m_events(new RetryingConnection(QStringLiteral("events"), this))
    , m_requests(new RetryingConnection(QStringLiteral("requests"), this))
{
    connect(m_events, &RetryingConnection::lineReceived, this, &IpcClient::handleEventLine);
    connect(m_requests, &RetryingConnection::lineReceived, this,
        [this](const QByteArray &line) { m_replyLines.enqueue(line); });
    connect(m_requests, &RetryingConnection::lineReceived, this, &IpcClient::pumpReplies);

    connect(m_events, &RetryingConnection::connected, this, [this] {
        // Handshake: niri replies Handled and then switches the socket into event
        // mode. This must be the only thing we ever write on this socket.
        m_events->connection()->send(RequestKind::EventStream);
        Q_EMIT eventStreamConnected();
    });

    connect(m_events, &RetryingConnection::disconnected, this,
        &IpcClient::eventStreamDisconnected);
    connect(m_requests, &RetryingConnection::disconnected, this, [this](const QString &why) {
        // Every outstanding request is now unanswerable. Complete them with an
        // error so callers do not leak closures.
        while (!m_handlers.isEmpty()) {
            Reply r;
            r.ok = false;
            r.errorMessage = QStringLiteral("niri connection lost: %1").arg(why);
            const ReplyHandler handler = m_handlers.dequeue();
            if (handler) {
                handler(r);
            }
        }
        m_requestsDead = true;
    });

    connect(m_requests, &RetryingConnection::connected, this, [this] {
        m_requestsDead = false;
        // Ask for the version once per connection so the log and the capability
        // report are accurate after a compositor restart.
        if (!m_versionRequested) {
            sendWithReply(RequestKind::Version, {}, [this](const Reply &r) {
                if (!r.ok) {
                    return;
                }
                const VersionSupport v =
                    VersionSupport::parse(r.payload.value(QStringLiteral("Version")).toString());
                if (!v.parsed) {
                    LOG_WARNF(Niri, "could not parse niri version '%s'", qPrintable(v.raw));
                } else if (v.belowMinimum) {
                    LOG_WARNF(Niri, "niri %s is older than the supported minimum %s: %s",
                        qPrintable(v.raw), qPrintable(QStringLiteral(KAPAH_NIRI_MIN_VERSION)),
                        qPrintable(v.note));
                } else if (v.aboveTested) {
                    LOG_WARNF(Niri, "niri %s: %s", qPrintable(v.raw), qPrintable(v.note));
                } else {
                    LOG_INFOF(Niri, "niri version %s", qPrintable(v.raw));
                }
                m_versionRequested = true;
                Q_EMIT versionReceived(v.raw);
            });
        }
    });
}

IpcClient::~IpcClient() = default;

void IpcClient::start()
{
    // Connect both immediately. Neither blocks: a Unix socket connect to a live
    // listener completes synchronously.
    m_events->connection()->open();
    m_requests->connection()->open();
}

void IpcClient::send(RequestKind kind, const QJsonObject &payload)
{
    m_requests->connection()->send(kind, payload);
}

void IpcClient::sendWithReply(RequestKind kind, const QJsonObject &payload, ReplyHandler handler)
{
    if (!handler) {
        send(kind, payload);
        return;
    }
    if (m_requestsDead || !m_requests->connection()->isConnected()) {
        Reply r;
        r.ok = false;
        r.errorMessage = QStringLiteral("not connected to niri");
        handler(r);
        return;
    }
    m_handlers.enqueue(std::move(handler));
    m_requests->connection()->send(kind, payload);
}

void IpcClient::action(const QJsonObject &actionJson)
{
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("Action"), actionJson);
    send(RequestKind::Action, wrapper);
}

void IpcClient::actionWithReply(const QJsonObject &actionJson, ReplyHandler handler)
{
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("Action"), actionJson);
    sendWithReply(RequestKind::Action, wrapper, std::move(handler));
}

void IpcClient::handleEventLine(const QByteArray &line)
{
    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_WARNF(Niri, "event stream: not JSON (%s): %s", qPrintable(err.errorString()),
            line.left(200).constData());
        return;
    }

    const QJsonObject obj = doc.object();
    const QStringList keys = obj.keys();
    if (keys.isEmpty()) {
        return;
    }

    // The handshake reply ("Handled") arrives on this socket before any event.
    if (keys.size() == 1 && keys.first() == QLatin1String("Ok")
        && obj.value(QLatin1String("Ok")).isString()
        && obj.value(QLatin1String("Ok")).toString() == QLatin1String("Handled")) {
        LOG_INFO(Niri, "event stream established");
        return;
    }

    if (keys.first() == QLatin1String("Err")) {
        LOG_WARNF(Niri, "niri refused the event stream: %s",
            qPrintable(obj.value(QLatin1String("Err")).toString()));
        return;
    }

    bool ok = false;
    const Event ev = Event::fromJson(obj, &ok);
    if (!ok) {
        return;
    }
    if (ev.kind == EventKind::Unknown) {
        // Version tolerance: log once per unknown variant.
        static QHash<QString, bool> reported;
        if (!reported.contains(ev.unknownKind)) {
            reported.insert(ev.unknownKind, true);
            LOG_WARNF(Niri,
                "ignoring unknown niri event '%s'. This niri is newer than %s; the shell "
                "keeps working without it.",
                qPrintable(ev.unknownKind), qPrintable(QStringLiteral(KAPAH_NIRI_TESTED_VERSION)));
        }
        return;
    }

    Q_EMIT eventReceived(ev);
}

void IpcClient::pumpReplies()
{
    while (true) {
        const QByteArray line = m_replyLines.isEmpty() ? QByteArray() : m_replyLines.dequeue();
        if (line.isEmpty()) {
            return;
        }

        QJsonParseError err {};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            LOG_WARNF(Niri, "reply was not JSON (%s): %s", qPrintable(err.errorString()),
                line.left(200).constData());
            continue;
        }

        const Reply reply = Reply::fromJson(doc.object());
        if (!reply.ok) {
            LOG_WARNF(Niri, "niri replied with an error: %s", qPrintable(reply.errorMessage));
            Q_EMIT requestFailed(reply.errorMessage);
        }

        if (m_handlers.isEmpty()) {
            // An unsolicited reply; nothing to complete. Happens only if a caller
            // used send() where sendWithReply() was meant.
            continue;
        }
        const ReplyHandler handler = m_handlers.dequeue();
        if (handler) {
            handler(reply);
        }
    }
}

} // namespace kapah::niri

#include "ipcclient.moc"