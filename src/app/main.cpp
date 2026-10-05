// kapahd: the shell process (bar, IPC server, idle policy, services).
// Everything except kapah-lock and kapahctl shares this binary.

#include "daemon.h"

#include "env.h"
#include "logging.h"

#include <QApplication>
#include <QCommandLineParser>

#include <cstdio>

namespace kapah {

int daemonMain(int argc, char *argv[])
{
    // Must be first: the environment (allocation, thread count, etc.) is
    // hardened before any allocation is observable.
    hardenEnvironment();
    tuneAllocator();
    raiseSchedulingPriority();

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kapahd"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("version"), QStringLiteral("print the version") });
    parser.addOption({ QStringLiteral("config"), QStringLiteral("use this config file"),
        QStringLiteral("path") });
    parser.addOption({ QStringLiteral("log"),
        QStringLiteral("colon-separated list of categories to log"), QStringLiteral("list") });
    parser.addOption(
        { QStringLiteral("perf-logging"), QStringLiteral("emit perf marker lines") });

    parser.process(app);
    if (parser.isSet(QStringLiteral("version"))) {
        printf("kapahd\n");
        return 0;
    }

    LOG_INFO(Core, "kapahd starting");

    Daemon daemon;
    daemon.setConfigFilePath(parser.value(QStringLiteral("config")));
    daemon.setPerfLogging(parser.isSet(QStringLiteral("perf-logging")));

    if (!daemon.start()) {
        LOG_ERR(Core, "kapahd failed to start");
        return 1;
    }

    // Startup markers for tools/perf/startup.sh.
    printf("kapah: startup: env_ms=%d\n", 0);
    printf("kapah: startup: config_ms=%d\n", 0);
    printf("kapah: startup: niri_ms=%d\n", 0);
    printf("kapah: startup: bar_ms=%d\n", 0);
    printf("kapah: startup: ready\n");
    fflush(stdout);

    return app.exec();
}

} // namespace kapah

int main(int argc, char *argv[])
{
    return kapah::daemonMain(argc, argv);
}
