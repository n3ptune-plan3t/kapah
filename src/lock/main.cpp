// kapah-lock: the session lock process.
//
// A separate binary, per ADR-001 / ADR-002 and docs/SECURITY.md: a crash in
// the main shell can never release the session lock, because the lock state
// belongs to the compositor, not to this process. This binary owns:
//   * the ext-session-lock-v1 lock (SessionLock), one surface per output,
//     painted opaque from an ARGB8888 shm frame only (LockView),
//   * the PAM conversation on a dedicated QThread (PamAuthenticator),
//   * raw wl_keyboard -> xkbcommon input (LockKeyboard),
//   * keyboard layout state from the niri event stream (NiriState).
//
// Lifetime is intentionally *not* tied to kapahd: kapah-lock is spawned at
// lock time and exits itself after authentication. If kapahd dies, this
// process keeps running and the session remains locked -- the safe direction.

#include "keyboard.h"
#include "lockview.h"
#include "pam.h"
#include "sessionlock.h"

#include "ipcclient.h"
#include "state.h"

#include "env.h"
#include "globals.h"
#include "logging.h"
#include "paths.h"
#include "theme.h"

#include <QApplication>
#include <QScreen>

#include <QByteArray>
#include <QCommandLineParser>

#include <cstdlib>

#include <wayland-client.h>

#include <QtWaylandClient/private/qwaylandintegration_p.h>

namespace kapah {

int lockMain(int argc, char *argv[])
{
    // env_ms: every entry point hardens the environment before anything else.
    hardenEnvironment();
    tuneAllocator();

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kapah-lock"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("version"), QStringLiteral("print the version") });
    parser.addOption({ QStringLiteral("password-timeout-ms"),
        QStringLiteral("reserved: bound the wait for a password attempt (0 = forever)"),
        QStringLiteral("ms") });
    parser.process(app);
    if (parser.isSet(QStringLiteral("version"))) {
        // No version header include in scope; keep it simple.
        printf("kapah-lock\n");
        return 0;
    }
    const qint64 timeoutMs = parser.value(QStringLiteral("password-timeout-ms")).toLongLong();
    Q_UNUSED(timeoutMs)

    LOG_INFO(Lock, "kapah-lock starting");

    Globals globals(&app);
    globals.start();

    SessionLock sessionLock;
    if (!sessionLock.begin()) {
        LOG_ERR(Lock, "cannot bind ext_session_lock_manager_v1; not locking. The session is "
            "left as-is, which is the compositor's previous state");
        return 2;
    }

    Theme theme;
    niri::IpcClient niriClient;
    niri::NiriState niriState;
    niriState.attach(&niriClient);
    niriClient.start();

    PamAuthenticator pam(paths::userName());
    LockView view(&sessionLock, &niriState, &theme, &pam);

    // The wl_display is the one Qt created for the wayland QPA platform.
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    auto *wd = integration != nullptr ? integration->display() : nullptr;
    if (wd == nullptr || wd->display() == nullptr) {
        LOG_ERR(Lock, "no wl_display: not running under a Wayland compositor");
        return 3;
    }

    LockKeyboard keyboard(globals.seat());
    view.setKeyboard(&keyboard);

    for (QScreen *screen : QGuiApplication::screens()) {
        view.addScreen(screen);
    }
    QObject::connect(&app, &QGuiApplication::screenAdded, &view,
        [&view](QScreen *s) { view.addScreen(s); });
    QObject::connect(&app, &QGuiApplication::screenRemoved, &view,
        [&view](QScreen *s) { view.removeScreen(s); });

    // If niri kills the lock (another lock took over, or it is shutting down),
    // we have no more session to protect: exit, leaving the state in niri's
    // hands.
    QObject::connect(&sessionLock, &SessionLock::lockLost, &app,
        [&app](const QString &reason) {
            LOG_WARNF(Lock, "session lock released by the compositor: %s",
                qPrintable(reason));
            app.exit(0);
        });

    return app.exec();
}

} // namespace kapah

int main(int argc, char *argv[])
{
    return kapah::lockMain(argc, argv);
}
