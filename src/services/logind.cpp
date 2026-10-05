#include "logind.h"

#include "logging.h"
#include "paths.h"
#include "util.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>

#include <unistd.h>

namespace kapah {

namespace {

const char *kManagerService = "org.freedesktop.login1";
const char *kManagerPath = "/org/freedesktop/login1";
const char *kManagerInterface = "org.freedesktop.login1.Manager";
const char *kSessionInterface = "org.freedesktop.login1.Session";

// logind names its properties per the D-Bus property conventions below.
const char *kSessionBrightness = "Brightness";
const char *kSessionCanSuspend = "CanSuspend";

} // namespace

void LogindService::Inhibitor::release()
{
    if (m_fd >= 0) {
        // Closing the fd is the entire release protocol. No D-Bus call.
        ::close(m_fd);
        m_fd = -1;
    }
}

// ---------------------------------------------------------------------------

LogindService::LogindService(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<QVariantMap>();
}

LogindService::~LogindService() = default;

void LogindService::start()
{
    const QString id = paths::logindSessionId();
    if (id.isEmpty()) {
        LOG_WARN(Core,
            "XDG_SESSION_ID is not set: no logind, so no unprivileged brightness writes, "
            "no idle inhibitors and no suspend/hibernate buttons. The shell works without "
            "them.");
        return;
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        LOG_WARN(Core, "no session bus: logind integration disabled");
        return;
    }

    m_sessionPath = QStringLiteral("/org/freedesktop/login1/session/") + id;

    // Name watcher first: if logind is not on the bus at all we stop here.
    auto *nameWatcher = new QDBusServiceWatcher(
        QString::fromLatin1(kManagerService), bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(nameWatcher, &QDBusServiceWatcher::serviceRegistered, this,
        [this] { parseProperties(); });
    connect(nameWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (!m_available) {
            return;
        }
        m_available = false;
        Q_EMIT availableChanged(false);
    });

    // Property watcher on the session object: brightness, idle hint and lid
    // switch all arrive as PropertiesChanged, which is the event-driven path.
    m_watcher = new QDBusObjectWatcher(
        m_sessionPath, bus, QDBusConnection::sessionBus().interface(), this);
    connect(m_watcher, &QDBusObjectWatcher::propertiesChanged, this,
        &LogindService::onPropertiesChanged);
    connect(m_watcher, &QDBusObjectWatcher::serviceOwnerChanged, this,
        &LogindService::onNameOwnerChanged);

    // PropertiesChanged does not cover signals, so the session's own signals
    // come through explicit bus connects, made once here in start() (not lazily
    // from the property callback).
    // PrepareForSleep is emitted by the *manager* object
    // (/org/freedesktop/login1, org.freedesktop.login1.Manager), not the
    // session object -- connecting it on m_sessionPath would never fire.
    bus.connect(QString::fromLatin1(kManagerService), QString::fromLatin1(kManagerPath),
        QString::fromLatin1(kManagerInterface), QStringLiteral("PrepareForSleep"), this,
        [this](const QVariant &when) {
            if (when.toBool()) {
                Q_EMIT aboutToSuspend();
            }
        });
    bus.connect(QString::fromLatin1(kManagerService), m_sessionPath,
        QString::fromLatin1(kSessionInterface), QStringLiteral("LockHint"), this,
        [this](const QVariant &hint) { Q_EMIT lockHintChanged(hint.toBool()); });

    parseProperties();
}

void LogindService::onNameOwnerChanged(const QString &service, const QString &oldOwner,
    const QString &newOwner)
{
    Q_UNUSED(service)
    Q_UNUSED(oldOwner)
    if (!newOwner.isEmpty() && !m_available) {
        parseProperties();
    } else if (newOwner.isEmpty() && m_available) {
        m_available = false;
        Q_EMIT availableChanged(false);
    }
}

void LogindService::parseProperties()
{
    auto bus = QDBusConnection::sessionBus();
    auto *iface = bus.interface();

    QDBusMessage sessionProps = QDBusMessage::createMethodCall(QString::fromLatin1(kManagerService),
        m_sessionPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    sessionProps.setArguments({ QString::fromLatin1(kSessionInterface) });

    auto *watcher = new QDBusPendingCallWatcher(iface->asyncCall(sessionProps, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher](
                                                               QDBusPendingCallReply<QVariantMap> reply) {
        watcher->deleteLater();
        if (!reply.isError()) {
            onPropertiesChanged(reply.value(), {});
            m_available = true;
            Q_EMIT availableChanged(true);
            Q_EMIT capabilitiesChanged();
        } else {
            LOG_WARNF(Core, "logind session properties unavailable: %s",
                qPrintable(reply.error().message()));
        }
    });

    QDBusMessage managerProps = QDBusMessage::createMethodCall(
        QString::fromLatin1(kManagerService), QString::fromLatin1(kManagerPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    managerProps.setArguments({ QString::fromLatin1(kManagerInterface) });

    auto *mw = new QDBusPendingCallWatcher(iface->asyncCall(managerProps, 3000), this);
    connect(mw, &QDBusPendingCallWatcher::finished, this, [mw](QDBusPendingCallReply<QVariantMap> reply) {
        mw->deleteLater();
        if (reply.isError()) {
            return;
        }
        const QVariantMap props = reply.value();
        m_canSuspend = props.value(QStringLiteral("CanSuspend"), false).toBool();
        m_canHibernate = props.value(QStringLiteral("CanHibernate"), false).toBool();
        m_canPowerOff = props.value(QStringLiteral("CanPowerOff"), true).toBool();
        m_canReboot = props.value(QStringLiteral("CanReboot"), true).toBool();
        Q_EMIT capabilitiesChanged();
    });
}

void LogindService::onPropertiesChanged(
    const QVariantMap &changed, const QVariantMap &invalidated)
{
    Q_UNUSED(invalidated)

    // Signals logind sends on the session object.
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
        if (it.key() == QLatin1String(kSessionBrightness)) {
            m_brightness = it.value().toInt();
        }
    }

    Q_EMIT capabilitiesChanged();
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

void LogindService::setBrightness(int percent, std::function<void(bool)> done)
{
    percent = qBound(0, percent, 100);
    if (!m_available) {
        if (done) {
            done(false);
        }
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kManagerService),
        m_sessionPath, QString::fromLatin1(kSessionInterface), QStringLiteral("SetBrightness"));
    msg.setArguments({ QStringLiteral("light"), static_cast<quint32>(percent), QString() });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [w, done](QDBusPendingCallReply<> reply) {
        w->deleteLater();
        const bool ok = !reply.isError();
        if (!ok) {
            LOG_WARNF(Backlight, "logind SetBrightness failed: %s",
                qPrintable(reply.error().message()));
        }
        if (done) {
            done(ok);
        }
    });
}

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------

namespace {

void callManagerMethod(const QString &method, std::function<void(bool)> done, QObject *owner)
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kManagerService),
        QString::fromLatin1(kManagerPath), QString::fromLatin1(kManagerInterface), method);
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 5000), owner);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, owner,
        [w, done, method](QDBusPendingCallReply<> reply) {
            w->deleteLater();
            const bool ok = !reply.isError();
            if (!ok) {
                LOG_WARNF(Power, "logind %s failed: %s", qPrintable(method),
                    qPrintable(reply.error().message()));
            }
            if (done) {
                done(ok);
            }
        });
}

} // namespace

void LogindService::suspend(std::function<void(bool)> done)
{
    callManagerMethod(QStringLiteral("Suspend"), std::move(done), this);
}

void LogindService::hibernate(std::function<void(bool)> done)
{
    callManagerMethod(QStringLiteral("Hibernate"), std::move(done), this);
}

void LogindService::powerOff(std::function<void(bool)> done)
{
    callManagerMethod(QStringLiteral("PowerOff"), std::move(done), this);
}

void LogindService::reboot(std::function<void(bool)> done)
{
    callManagerMethod(QStringLiteral("Reboot"), std::move(done), this);
}

void LogindService::lockSession(std::function<void(bool)> done)
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kManagerService),
        m_sessionPath, QString::fromLatin1(kSessionInterface), QStringLiteral("Lock"));
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [w, done](QDBusPendingCallReply<> reply) {
        w->deleteLater();
        if (done) {
            done(!reply.isError());
        }
    });
}

// ---------------------------------------------------------------------------
// Inhibitors
// ---------------------------------------------------------------------------

void LogindService::takeInhibitor(const QString &what, const QString &who, const QString &why,
    InhibitorMode mode, std::function<void(Inhibitor)> done)
{
    if (!m_available) {
        if (done) {
            done(Inhibitor {});
        }
        return;
    }

    const char *modeStr = "block";
    switch (mode) {
    case InhibitorMode::Block:
        modeStr = "block";
        break;
    case InhibitorMode::Ignore:
        modeStr = "ignore";
        break;
    case InhibitorMode::Delay:
        modeStr = "delay";
        break;
    }

    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kManagerService),
        QString::fromLatin1(kManagerPath), QString::fromLatin1(kManagerInterface),
        QStringLiteral("Inhibit"));
    msg.setArguments({ what, who, why, QString::fromLatin1(modeStr) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [w, done](QDBusPendingCallReply<> reply) {
        w->deleteLater();
        if (reply.isError()) {
            LOG_WARNF(Power, "logind Inhibit failed: %s", qPrintable(reply.error().message()));
            if (done) {
                done(Inhibitor {});
            }
            return;
        }
        const uint32_t fd = reply.value().value<uint32_t>();
        if (done) {
            done(Inhibitor(static_cast<int>(fd)));
        }
    });
}

} // namespace kapah