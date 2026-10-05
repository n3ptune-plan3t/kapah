// kapahctl: the tiny CLI. Qt Core only, no GUI module. Starts in < 5 ms by
// parsing argv by hand and using a raw socket in non-blocking mode -- no
// QGuiApplication, no event loop.

#include "kapah/version.h"
#include "paths.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSocketNotifier>
#include <QCoreApplication>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

int connectUnix(const char *path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0
        && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void printUsage()
{
    std::fputs("usage: kapahctl <command> [args...]\n"
               "  reload | quit | status | perf\n"
               "  volume up|down|mute [step]\n"
               "  brightness up|down [step]\n"
               "  launcher|osd|notify|powermenu|quicksettings|lock ... (not implemented yet)\n",
        stderr);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kapahctl"));

    if (argc < 2) {
        printUsage();
        return 2;
    }

    // Build the request JSON: {"major":1,"cmd":"<first>","args":[...]}
    QJsonObject req;
    req[QStringLiteral("major")] = KAPAH_IPC_VERSION_MAJOR;
    req[QStringLiteral("cmd")] = QString::fromLocal8Bit(argv[1]);
    QJsonArray args;
    for (int i = 2; i < argc; ++i) {
        args.append(QString::fromLocal8Bit(argv[i]));
    }
    req[QStringLiteral("args")] = args;

    const QString socketPath = kapah::paths::controlSocket();
    const QByteArray path = socketPath.toLocal8Bit();
    const int fd = connectUnix(path.constData());
    if (fd < 0) {
        std::fprintf(stderr, "kapahctl: cannot connect to %s: %s\n", path.constData(),
            std::strerror(errno));
        return 1;
    }

    // Wait briefly for the non-blocking connect to finish, then send.
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv { 1, 0 };
    if (::select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) {
        std::fprintf(stderr, "kapahctl: timeout connecting\n");
        ::close(fd);
        return 1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        std::fprintf(stderr, "kapahctl: connect failed: %s\n", std::strerror(err));
        ::close(fd);
        return 1;
    }

    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    if (::write(fd, line.constData(), static_cast<size_t>(line.size())) < 0) {
        std::fprintf(stderr, "kapahctl: write failed: %s\n", std::strerror(errno));
        ::close(fd);
        return 1;
    }

    // Read one response line.
    QByteArray in;
    char buf[256];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            in.append(buf, static_cast<int>(n));
            if (in.contains('\n')) {
                break;
            }
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(fd);

    if (in.isEmpty()) {
        std::fputs("kapahctl: no response\n", stderr);
        return 1;
    }

    // Print the response body to stdout; exit non-zero on ok==false.
    const QJsonDocument doc = QJsonDocument::fromJson(in);
    const bool ok = doc.object().value(QStringLiteral("ok")).toBool(false);
    if (!ok) {
        std::fprintf(stderr, "kapahctl: %s\n",
            doc.object().value(QStringLiteral("error")).toString().toLocal8Bit().constData());
        return 1;
    }
    // The JSON dump (status) goes to stdout verbatim.
    std::fwrite(in.constData(), 1, static_cast<size_t>(in.size()), stdout);
    return 0;
}
