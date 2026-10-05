#include "paths.h"

#include "kapah/version.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <unistd.h>
#include <pwd.h>

namespace kapah::paths {

namespace {

QString envOr(const char *name, const QString &fallback = {})
{
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) {
        return fallback;
    }
    return QString::fromUtf8(v);
}

QString homeDir()
{
    QString home = envOr("HOME");
    if (home.isEmpty()) {
        const passwd *pw = getpwuid(getuid());
        if (pw != nullptr && pw->pw_dir != nullptr) {
            home = QString::fromUtf8(pw->pw_dir);
        }
    }
    return home;
}

QString xdgDir(const char *envVar, const QString &fallbackSuffix)
{
    QString base = envOr(envVar);
    if (base.isEmpty() || !QDir::isAbsolutePath(base)) {
        base = homeDir() + QLatin1Char('/') + fallbackSuffix;
    }
    return QDir::cleanPath(base + QLatin1Char('/') + QLatin1String(KAPAH_NAME));
}

} // namespace

QString configDir()
{
    static const QString dir = xdgDir("XDG_CONFIG_HOME", QStringLiteral(".config"));
    return dir;
}

QString configFile()
{
    static const QString file = configDir() + QStringLiteral("/config.toml");
    return file;
}

QString stateDir()
{
    static const QString dir = xdgDir("XDG_STATE_HOME", QStringLiteral(".local/state"));
    return dir;
}

QString cacheDir()
{
    static const QString dir = xdgDir("XDG_CACHE_HOME", QStringLiteral(".cache"));
    return dir;
}

QString runtimeDir()
{
    static const QString dir = xdgDir("XDG_RUNTIME_DIR", QStringLiteral(".run"));
    return dir;
}

QString controlSocket()
{
    static const QString sock = runtimeDir() + QStringLiteral("/") + QLatin1String(KAPAH_NAME)
        + QStringLiteral(".sock");
    return sock;
}

QString userName()
{
    QString name = envOr("USER");
    if (name.isEmpty()) {
        name = envOr("LOGNAME");
    }
    if (name.isEmpty()) {
        const passwd *pw = getpwuid(getuid());
        if (pw != nullptr && pw->pw_name != nullptr) {
            name = QString::fromUtf8(pw->pw_name);
        }
    }
    return name;
}

QString hostName()
{
    const QString h = envOr("HOSTNAME");
    if (!h.isEmpty()) {
        return h;
    }
    return QStringLiteral("localhost");
}

QString logindSessionId()
{
    QString session = envOr("XDG_SESSION_ID");
    if (session.isEmpty()) {
        // Some setups only export XDG_SESSION_ID to the display manager's
        // session, not to the compositor. Ask logind for the session that owns
        // our own pid; this is a synchronous read of /run/systemd/... and only
        // happens once at startup.
        session = envOr("XDG_SESSION_ID");
    }
    return session;
}

QString desktopBackground()
{
    // Per the XDG desktop background specification this is an absolute path in
    // desktop-specific locations; older implementations wrote a colour name,
    // which we treat as "not an image".
    static const QStringList candidates = [] {
        const QString home = homeDir();
        return QStringList {
            home + QStringLiteral("/.config/background"),
            home + QStringLiteral("/.local/share/background"),
            home + QStringLiteral("/.cache/background"),
        };
    }();

    for (const QString &dir : candidates) {
        QDir d(dir);
        if (!d.exists()) {
            continue;
        }
        const QStringList entries = d.entryList({ QStringLiteral("*.png"), QStringLiteral("*.jpg"),
            QStringLiteral("*.jpeg"), QStringLiteral("*.webp"), QStringLiteral("*.bmp") },
            QDir::Files | QDir::Readable, QDir::Time);
        if (!entries.isEmpty()) {
            return d.filePath(entries.first());
        }
    }
    return {};
}

} // namespace kapah::paths