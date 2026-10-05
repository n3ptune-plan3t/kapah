#include "network.h"

#include "logging.h"
#include "util.h"

#include <QDBusConnection>
#include <QDBusReply>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QTimer>

#include <cstring>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kapah {

namespace {

const char *kNmService = "org.freedesktop.NetworkManager";
const char *kNmPath = "/org/freedesktop/NetworkManager";
const char *kNmInterface = "org.freedesktop.NetworkManager";
const char *kNmDeviceInterface = "org.freedesktop.NetworkManager.Device";
const char *kNmDeviceWifi = "org.freedesktop.NetworkManager.Device.Wireless";
const char *kNmDeviceWired = "org.freedesktop.NetworkManager.Device.Wired";
const char *kNmActiveConnection = "org.freedesktop.NetworkManager.Connection.Active";
const char *kNmDeviceWifiAp = "org.freedesktop.NetworkManager.AccessPoint";
const char *kNmSettings = "org.freedesktop.NetworkManager.Settings";
const char *kNmSettingsConnection = "org.freedesktop.NetworkManager.Settings.Connection";
const char *kProps = "org.freedesktop.DBus.Properties";

NetworkKind kindFromNmType(const QString &type)
{
    if (type == QLatin1String("ethernet") || type == QLatin1String("802-3-ethernet")) {
        return NetworkKind::Wired;
    }
    if (type == QLatin1String("wifi")) {
        return NetworkKind::Wireless;
    }
    if (type == QLatin1String("bluetooth")) {
        return NetworkKind::Bluetooth;
    }
    if (type == QLatin1String("vpn")) {
        return NetworkKind::Vpn;
    }
    if (type == QLatin1String("loopback")) {
        return NetworkKind::None;
    }
    return NetworkKind::Wired;
}

NetworkKind kindFromSysfsType(const QString &type)
{
    if (type.isEmpty()) {
        return NetworkKind::Wired;
    }
    if (type == QLatin1String("wireless")) {
        return NetworkKind::Wireless;
    }
    if (type.contains(QLatin1String("802-3-ethernet")) || type.contains(QLatin1String("ethernet"))) {
        return NetworkKind::Wired;
    }
    return NetworkKind::Wired;
}

} // namespace

NetworkService::NetworkService(QObject *parent)
    : QObject(parent)
{
}

NetworkService::~NetworkService()
{
    stop();
}

void NetworkService::start(const QStringList &backends)
{
    bool wantNm = backends.isEmpty();
    bool wantNetlink = backends.isEmpty();
    for (const QString &b : backends) {
        const QString l = b.trimmed().toLower();
        if (l == QLatin1String("networkmanager")) {
            wantNm = true;
        } else if (l == QLatin1String("netlink")) {
            wantNetlink = true;
        }
    }

    if (wantNetlink) {
        startNetlink();
    }
    if (wantNm) {
        startNetworkManager();
    }

    if (m_netlinkEnabled || m_nmEnabled) {
        m_available = true;
        m_backend = m_nmReady ? QStringLiteral("networkmanager")
                             : (m_netlinkEnabled ? QStringLiteral("netlink") : QString());
        LOG_INFOF(Network, "network backend: %s", qPrintable(m_backend));
        Q_EMIT availableChanged(true);
    } else {
        LOG_WARN(Network,
            "no network backend available: NetworkManager is not on the bus and netlink "
            "could not be opened. The network module shows 'unavailable'.");
    }
}

void NetworkService::stop()
{
    stopNetworkManager();
    if (m_netlinkNotifier != nullptr) {
        delete m_netlinkNotifier;
        m_netlinkNotifier = nullptr;
    }
    if (m_netlinkFd >= 0) {
        ::close(m_netlinkFd);
        m_netlinkFd = -1;
    }
}

QString NetworkService::backendName() const
{
    return m_backend;
}

// ---------------------------------------------------------------------------
// NetworkManager
// ---------------------------------------------------------------------------

void NetworkService::startNetworkManager()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }

    auto *watcher = new QDBusServiceWatcher(QString::fromLatin1(kNmService), bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
        m_nmReady = false;
        startNetworkManager();
    });
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        // NM went away: fall back to netlink, which never went anywhere.
        stopNetworkManager();
        if (m_netlinkEnabled) {
            m_backend = QStringLiteral("netlink");
            LOG_INFO(Network, "NetworkManager exited; falling back to netlink");
            Q_EMIT stateChanged();
        } else if (m_available) {
            m_available = false;
            Q_EMIT availableChanged(false);
        }
    });

    // Global signals: no polling, no timers.
    bus.connect(QString::fromLatin1(kNmService), QStringLiteral("/org/freedesktop/NetworkManager"),
        QString::fromLatin1(kNmInterface), QStringLiteral("StateChanged"), this,
        [this](quint32 newState) { onNmStateChanged(newState); });
    bus.connect(QString::fromLatin1(kNmService), QStringLiteral("/org/freedesktop/NetworkManager"),
        QString::fromLatin1(kNmInterface), QStringLiteral("DeviceAdded"), this,
        [this](const QDBusObjectPath &path, const QVariantMap &) { refreshNmDevice(path.path()); });
    bus.connect(QString::fromLatin1(kNmService), QStringLiteral("/org/freedesktop/NetworkManager"),
        QString::fromLatin1(kNmInterface), QStringLiteral("DeviceRemoved"), this,
        [this](const QDBusObjectPath &path) {
            m_interfaces.removeIf([&path](const NetworkInterface &i) {
                return i.name == QFileInfo(path.path()).fileName();
            });
            Q_EMIT interfacesChanged();
            recomputePrimary();
        });

    subscribeToSignals();
    refreshNmDevices();
}

void NetworkService::stopNetworkManager()
{
    m_nmReady = false;
    m_nmEnabled = false;
    m_accessPoints.clear();
}

void NetworkService::subscribeToSignals()
{
    auto bus = QDBusConnection::sessionBus();
    // NM advertises its own signal capabilities; subscribing is the correct way
    // to be told about everything without polling GetDevices.
    bus.connect(QString::fromLatin1(kNmService), QStringLiteral("/org/freedesktop/NetworkManager"),
        QString::fromLatin1(kNmInterface), QStringLiteral("PropertiesChanged"), this,
        [this](const QVariantMap &changed, const QVariantMap &) {
            if (changed.contains(QStringLiteral("State"))) {
                onNmStateChanged(changed.value(QStringLiteral("State")).toUInt());
            }
        });
}

void NetworkService::onNmStateChanged(quint32 newState)
{
    // NM_State enum, from NM's own header:
    //   0 unknown, 1 asleep, 2 disconnected, 3 disconnecting, 4 connecting,
    //   5 connected (local only), 6 connected (site), 7 connected (global)
    switch (newState) {
    case 0:
        m_state = NetworkState::Unknown;
        break;
    case 1:
    case 2:
    case 3:
        m_state = NetworkState::Offline;
        break;
    case 4:
        m_state = NetworkState::Connecting;
        break;
    case 5:
    case 6:
    case 7:
        m_state = NetworkState::Connected;
        break;
    default:
        m_state = NetworkState::Unknown;
        break;
    }

    refreshActiveConnection();
    refreshNmDevices();
    Q_EMIT stateChanged();
}

void NetworkService::refreshNmDevices()
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmPath), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    msg.setArguments({ QString() });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 4000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w](QDBusPendingCallReply<QDBusObjectPath, QMap<QString, QVariantMap>> reply) {
            w->deleteLater();
            if (reply.isError()) {
                LOG_WARNF(Network, "NetworkManager GetManagedObjects failed: %s",
                    qPrintable(reply.error().message()));
                return;
            }
            m_nmReady = true;
            if (!m_nmEnabled) {
                m_nmEnabled = true;
                m_backend = QStringLiteral("networkmanager");
            }

            QVector<NetworkInterface> found;
            const auto objects = reply.value().second;
            for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
                const QString path = it.key();
                const QVariantMap &ifaces = it.value();
                const auto devIt = ifaces.constFind(QString::fromLatin1(kNmDeviceInterface));
                if (devIt == ifaces.constEnd()) {
                    continue;
                }
                const QVariantMap props = devIt.value().toMap();

                NetworkInterface ni;
                ni.name = props.value(QStringLiteral("Interface")).toString();
                ni.typeName = props.value(QStringLiteral("DeviceType")).toString();
                ni.kind = kindFromNmType(ni.typeName);
                // NM_DeviceState: 0 unknown, 1 unmanaged, 2 unavailable, 3
                // disconnected, 4 preparing, 5 configuring, 6 needs-auth,
                // 7 ip-config, 8 ip-check, 9 secondaries, 10 activated,
                // 11 deactivating, 12 unavailable-failed.
                const quint32 devState = props.value(QStringLiteral("State"), 0u).toUInt();
                switch (devState) {
                case 10:
                    ni.state = NetworkState::Connected;
                    break;
                case 4:
                case 5:
                case 6:
                case 7:
                case 8:
                case 9:
                    ni.state = NetworkState::Connecting;
                    break;
                case 3:
                    ni.state = NetworkState::Offline;
                    break;
                default:
                    ni.state = NetworkState::Unknown;
                    break;
                }
                ni.carrier = props.value(QStringLiteral("Carrier")).toBool()
                    || props.value(QStringLiteral("CarrierChanges"), 0).toUInt() == 0;
                ni.hasAddresses = ni.state == NetworkState::Connected;
                ni.activeConnection =
                    props.value(QStringLiteral("ActiveConnection")).value<QDBusObjectPath>().path();
                ni.stateReason = props.value(QStringLiteral("StateReason")).toString();
                ni.signalStrength =
                    static_cast<int>(props.value(QStringLiteral("SignalStrength"), 0).toUInt());

                // Wireless extras live on the .Wireless interface.
                const auto wifiIt = ifaces.constFind(QString::fromLatin1(kNmDeviceWifi));
                if (wifiIt != ifaces.constEnd()) {
                    const QVariantMap wifi = wifiIt.value().toMap();
                    ni.kind = NetworkKind::Wireless;
                    QVariant ssid = wifi.value(QStringLiteral("Ssid"));
                    if (ssid.canConvert<QByteArray>()) {
                        ni.ssid = QString::fromUtf8(ssid.toByteArray());
                    }
                    ni.activeConnection =
                        wifi.value(QStringLiteral("ActiveAccessPoint")).value<QDBusObjectPath>().path();
                    m_wirelessEnabledByInterface.insert(ni.name,
                        wifi.value(QStringLiteral("WirelessEnabled"), true).toBool());
                }

                found.append(ni);
            }

            m_interfaces = found;
            Q_EMIT interfacesChanged();
            recomputePrimary();
            refreshActiveConnection();
        });
}

void NetworkService::refreshNmDevice(const QString &objectPath)
{
    // A device was added: one targeted read rather than a full rescan.
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        objectPath, QString::fromLatin1(kProps), QStringLiteral("GetAll"));
    msg.setArguments({ QString::fromLatin1(kNmDeviceInterface) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w](QDBusPendingCallReply<QVariantMap> reply) {
            w->deleteLater();
            if (reply.isError()) {
                return;
            }
            const QVariantMap props = reply.value();
            NetworkInterface ni;
            ni.name = props.value(QStringLiteral("Interface")).toString();
            ni.typeName = props.value(QStringLiteral("DeviceType")).toString();
            ni.kind = kindFromNmType(ni.typeName);
            ni.signalStrength =
                static_cast<int>(props.value(QStringLiteral("SignalStrength"), 0).toUInt());

            bool replaced = false;
            for (NetworkInterface &existing : m_interfaces) {
                if (existing.name == ni.name) {
                    const quint32 devState = props.value(QStringLiteral("State"), 0u).toUInt();
                    existing.state = devState == 10 ? NetworkState::Connected
                        : devState >= 4 && devState <= 9 ? NetworkState::Connecting
                                                          : NetworkState::Offline;
                    existing.carrier = props.value(QStringLiteral("Carrier")).toBool();
                    existing.signalStrength = ni.signalStrength;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                ni.state = NetworkState::Offline;
                m_interfaces.append(ni);
            }
            Q_EMIT interfacesChanged();
            recomputePrimary();
        });
}

void NetworkService::refreshActiveConnection()
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmPath), QString::fromLatin1(kNmInterface),
        QStringLiteral("GetActiveConnection"));
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w](QDBusPendingCallReply<QDBusObjectPath> reply) {
            w->deleteLater();
            const QString path = reply.isError() ? QString() : reply.value().path();
            if (path == m_activeConnectionPath) {
                return;
            }
            m_activeConnectionPath = path;
            m_activeConnectionId.clear();
            if (path.isEmpty()) {
                m_ssid.clear();
                m_signalStrength = 0;
                Q_EMIT stateChanged();
                return;
            }
            // One read for the connection id (which is the SSID for wifi).
            QDBusMessage msg2 = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                path, QString::fromLatin1(kProps), QStringLiteral("GetAll"));
            msg2.setArguments({ QString::fromLatin1(kNmActiveConnection) });
            auto *w2 = new QDBusPendingCallWatcher(bus.asyncCall(msg2, 2000), this);
            connect(w2, &QDBusPendingCallWatcher::finished, this,
                [this, w2](QDBusPendingCallReply<QVariantMap> r2) {
                    w2->deleteLater();
                    if (r2.isError()) {
                        return;
                    }
                    const QVariantMap props = r2.value();
                    m_activeConnectionId = props.value(QStringLiteral("Id")).toString();
                    const QDBusObjectPath devicePath =
                        props.value(QStringLiteral("Devices")).toList().isEmpty()
                        ? QDBusObjectPath()
                        : props.value(QStringLiteral("Devices")).toList().first().value<QDBusObjectPath>();
                    if (devicePath.isValid()) {
                        // The device list is the source of truth for kind/ssid/
                        // strength; for a wireless device its `activeConnection`
                        // holds the ActiveAccessPoint path, which is what
                        // marks AccessPoint::active in the merged view.
                        for (const NetworkInterface &ni : m_interfaces) {
                            if (ni.path == devicePath.path()) {
                                m_ssid = ni.ssid;
                                m_signalStrength = ni.signalStrength;
                                m_kind = ni.kind;
                                if (ni.kind == NetworkKind::Wireless) {
                                    m_activeApPath = ni.activeConnection;
                                }
                            }
                        }
                    }
                    Q_EMIT stateChanged();
                });
        });
}

// ---------------------------------------------------------------------------
// netlink fallback
// ---------------------------------------------------------------------------

void NetworkService::startNetlink()
{
    m_netlinkFd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (m_netlinkFd < 0) {
        LOG_WARN(Network, "cannot open a NETLINK_ROUTE socket");
        return;
    }

    struct sockaddr_nl addr {};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK; // link up/down events only
    if (::bind(m_netlinkFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        LOG_WARNF(Network, "cannot bind the netlink socket: %s", strerror(errno));
        ::close(m_netlinkFd);
        m_netlinkFd = -1;
        return;
    }

    m_netlinkEnabled = true;

    // Ask for the current set of links once, then wait for RTM_NEWLINK.
    struct {
        nlmsghdr header;
        rtgenmsg body;
    } request {};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.body));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.body.rtgen_family = AF_UNSPEC;
    ::send(m_netlinkFd, &request, request.header.nlmsg_len, 0);

    m_netlinkNotifier = new QSocketNotifier(m_netlinkFd, QSocketNotifier::Read, this);
    connect(m_netlinkNotifier, &QSocketNotifier::activated, this, &NetworkService::readNetlink);

    readNetlink();
    readSysfsInterfaces();
}

void NetworkService::readNetlink()
{
    if (m_netlinkFd < 0) {
        return;
    }
    // Drain fully: a level-triggered notifier costs one wakeup per read if we
    // return early, and a link event is cheap to read.
    for (int guard = 0; guard < 64; ++guard) {
        char buffer[8192];
        const ssize_t n = ::recv(m_netlinkFd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }

        const auto *header = reinterpret_cast<nlmsghdr *>(buffer);
        for (nlmsghdr *h = header; NLMSG_OK(h, static_cast<unsigned int>(n));
             h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) {
                break;
            }
            if (h->nlmsg_type != RTM_NEWLINK && h->nlmsg_type != RTM_DELLINK) {
                continue;
            }
            const auto *ifInfo = static_cast<const struct ifinfomsg *>(NLMSG_DATA(h));
            if (ifInfo->ifi_index == 0) {
                continue;
            }
            readSysfsInterfaces();
        }
    }

    Q_EMIT interfacesChanged();
    recomputePrimary();
    Q_EMIT stateChanged();
}

void NetworkService::readSysfsInterfaces()
{
    QVector<NetworkInterface> found;
    QDir net("/sys/class/net");
    const auto entries = net.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
    for (const QString &name : entries) {
        const QString dir = net.filePath(name);
        const QString typeSymlink = dir + QStringLiteral("/device/type");
        Q_UNUSED(typeSymlink)

        QString wireless;
        if (util::readSysfs(dir + QStringLiteral("/wireless/link"), nullptr) || true) {
            util::readSysfs(dir + QStringLiteral("/wireless/status"), &wireless);
        }

        NetworkInterface ni;
        ni.name = name;
        ni.carrier = false;
        // operstate: up, down, unknown, dormant, lowerlayerdown
        QString operstate;
        if (util::readSysfs(dir + QStringLiteral("/operstate"), &operstate)) {
            ni.carrier = operstate == QLatin1String("up");
        }
        if (!wireless.isEmpty()) {
            ni.kind = NetworkKind::Wireless;
        } else {
            // /sys/class/net/*/type == 1 is ARPHRD_ETHER.
            int type = 0;
            if (util::readSysfsInt(dir + QStringLiteral("/type"), &type)) {
                ni.kind = kindFromSysfsType(type == 1 ? QStringLiteral("ethernet") : QString());
            }
        }
        ni.state = ni.carrier ? NetworkState::Connected : NetworkState::Offline;
        ni.hasAddresses = ni.carrier;
        found.append(ni);
    }

    // Only replace when we are not the primary backend: NM has better information.
    if (!m_nmReady) {
        m_interfaces = found;
    } else {
        // Merge: keep NM's states, fill in anything NM does not know about.
        for (const NetworkInterface &nf : found) {
            bool known = false;
            for (const NetworkInterface &e : m_interfaces) {
                if (e.name == nf.name) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                m_interfaces.append(nf);
            }
        }
    }
}

void NetworkService::recomputePrimary()
{
    // Preference: the interface NM reports as active, then any connected wired
    // link, then any connected wireless, then anything with a carrier.
    QString best;
    int bestRank = -1;
    for (const NetworkInterface &ni : m_interfaces) {
        int rank = 0;
        if (ni.name == m_activeConnectionId) {
            rank = 100;
        } else if (ni.kind == NetworkKind::Wired && ni.state == NetworkState::Connected) {
            rank = 50;
        } else if (ni.kind == NetworkKind::Wireless && ni.state == NetworkState::Connected) {
            rank = 40;
        } else if (ni.state == NetworkState::Connecting) {
            rank = 30;
        } else if (ni.carrier) {
            rank = 10;
        }
        if (rank > bestRank) {
            bestRank = rank;
            best = ni.name;
        }
    }

    if (best != m_primary) {
        m_primary = best;
        Q_EMIT interfacesChanged();
    }

    // Kind and SSID follow the primary interface.
    NetworkKind newKind = NetworkKind::None;
    QString newSsid;
    int newSignal = 0;
    for (const NetworkInterface &ni : m_interfaces) {
        if (ni.name == m_primary) {
            newKind = ni.kind;
            newSsid = ni.ssid;
            newSignal = ni.signalStrength;
            break;
        }
    }
    // If NM says we are globally connected but no device matched, report wired:
    // that is the common "unknown device type" case.
    if (newKind == NetworkKind::None && m_state == NetworkState::Connected) {
        newKind = NetworkKind::Wired;
    }
    if (m_kind != newKind || m_ssid != newSsid || m_signalStrength != newSignal) {
        m_kind = newKind;
        m_ssid = newSsid;
        m_signalStrength = newSignal;
        Q_EMIT stateChanged();
    }

    // With only the netlink backend, the state *is* the carrier state.
    if (!m_nmReady && m_netlinkEnabled) {
        NetworkState computed = NetworkState::Offline;
        for (const NetworkInterface &ni : m_interfaces) {
            if (ni.name == m_primary && ni.carrier) {
                computed = NetworkState::Connected;
                break;
            }
        }
        if (computed != m_state) {
            m_state = computed;
            Q_EMIT stateChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi operations
// ---------------------------------------------------------------------------

bool NetworkService::hasWifi() const
{
    for (const NetworkInterface &ni : m_interfaces) {
        if (ni.kind == NetworkKind::Wireless) {
            return true;
        }
    }
    return false;
}

void NetworkService::requestScan(std::function<void()> finished)
{
    if (!m_nmReady || m_scanInFlight) {
        if (finished) {
            finished();
        }
        return;
    }
    m_scanInFlight = true;
    auto bus = QDBusConnection::sessionBus();

    // Step 1: find the wireless device's object path. NM does not expose it by
    // interface name, so we walk the managed objects once -- but only because the
    // caller asked for a scan, i.e. only while the wifi selector is open.
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmPath), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    msg.setArguments({ QString() });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, finished](QDBusPendingCallReply<QDBusObjectPath, QMap<QString, QVariantMap>> r) {
            w->deleteLater();
            QString wifiPath;
            if (!r.isError()) {
                const auto objects = r.value().second;
                for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
                    if (it.value().contains(QString::fromLatin1(kNmDeviceWifi))) {
                        wifiPath = it.key();
                        break;
                    }
                }
            }
            if (wifiPath.isEmpty()) {
                m_scanInFlight = false;
                if (finished) {
                    finished();
                }
                return;
            }

            // Step 2: ask that device for its AP list, then read each AP's
            // properties. Each read is a pending call so nothing blocks.
            QDBusMessage req = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                wifiPath, QString::fromLatin1(kNmDeviceWifi), QStringLiteral("GetAllAccessPoints"));
            auto *w2 = new QDBusPendingCallWatcher(bus.asyncCall(req, 5000), this);
            connect(w2, &QDBusPendingCallWatcher::finished, this,
                [this, w2, finished](QDBusPendingCallReply<QList<QDBusObjectPath>> r2) {
                    w2->deleteLater();
                    m_scanInFlight = false;
                    if (!r2.isError()) {
                        m_accessPoints.clear();
                        for (const QDBusObjectPath &ap : r2.value()) {
                            readAccessPoint(ap.path());
                        }
                    }
                    if (finished) {
                        finished();
                    }
                });
        });
}

void NetworkService::readAccessPoint(const QString &apPath)
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService), apPath,
        QString::fromLatin1(kProps), QStringLiteral("GetAll"));
    m.setArguments({ QString::fromLatin1(kNmDeviceWifiAp) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m, 2000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, apPath](QDBusPendingCallReply<QVariantMap> reply) {
            w->deleteLater();
            if (reply.isError()) {
                return;
            }
            const QVariantMap p = reply.value();
            AccessPoint a;
            a.ssid = QString::fromUtf8(p.value(QStringLiteral("Ssid")).toByteArray());
            a.bssid = p.value(QStringLiteral("Bssid")).toString();
            a.frequency = static_cast<quint32>(p.value(QStringLiteral("Frequency"), 0).toUInt());
            // NM's Strength is 0-100 where lower is better.
            a.signalStrength = 100 - static_cast<int>(p.value(QStringLiteral("Strength"), 100).toUInt()) / 2;
            a.secured = p.value(QStringLiteral("Flags"), 0u).toUInt() != 0;
            a.path = apPath;
            a.band = a.frequency < 3000 ? 1 : (a.frequency < 5925 ? 2 : 3);
            a.active = (a.path == m_activeApPath);

            bool replaced = false;
            for (AccessPoint &existing : m_accessPoints) {
                if (existing.bssid == a.bssid) {
                    existing = a;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_accessPoints.append(a);
            }
            Q_EMIT stateChanged();
        });
}

QString NetworkService::wifiDevicePath()
{
    if (!m_nmReady) {
        return {};
    }
    // The managed-objects walk is only ever done from an open panel, so caching
    // it for the panel's lifetime is acceptable and avoids a second walk.
    if (!m_wifiDevicePath.isEmpty()) {
        return m_wifiDevicePath;
    }
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmPath), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    msg.setArguments({ QString() });

    // This is a blocking call, and it is only reached from the wifi selector's
    // construction path, which runs inside a single-shot timer on the panel-open
    // path rather than from a paint event. Documented as the one place
    // NetworkService blocks, per roadmap 4.4's exception process.
    QDBusMessage reply = bus.call(msg, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        return {};
    }
    const auto objects = qdbus_cast<QMap<QString, QVariantMap>>(reply.arguments().first());
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        if (it.value().contains(QString::fromLatin1(kNmDeviceWifi))) {
            m_wifiDevicePath = it.key();
            break;
        }
    }
    return m_wifiDevicePath;
}

void NetworkService::setWifiEnabledOnDevice(const QString &devicePath, bool enabled,
    std::function<void(bool)> done)
{
    if (!m_nmReady || devicePath.isEmpty()) {
        if (done) {
            done(false);
        }
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        devicePath, QString::fromLatin1(kProps), QStringLiteral("Set"));
    msg.setArguments({ QString::fromLatin1(kNmDeviceWifi), QStringLiteral("WirelessEnabled"),
        QVariant(enabled) });
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [w, done](QDBusPendingCallReply<> r) {
        w->deleteLater();
        const bool ok = !r.isError();
        if (done) {
            done(ok);
        }
    });
}

void NetworkService::activate(const QString &ssid, std::function<void(bool, const QString &)> done)
{
    if (!m_nmReady) {
        if (done) {
            done(false, QStringLiteral("NetworkManager is not available"));
        }
        return;
    }

    auto bus = QDBusConnection::sessionBus();

    // Step 1: does a saved connection exist for this SSID?
    QDBusMessage list = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmSettings), QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    list.setArguments(
        { QString::fromLatin1(kNmInterface), QStringLiteral("Connections") });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(list, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, ssid, done](QDBusPendingCallReply<QVariant> reply) {
            w->deleteLater();
            if (reply.isError()) {
                if (done) {
                    done(false, reply.error().message());
                }
                return;
            }
            const QStringList ids =
                reply.value().value<QStringList>();
            QString found;
            for (const QString &id : ids) {
                if (id.contains(ssid, Qt::CaseInsensitive)) {
                    found = id;
                    break;
                }
            }
            if (found.isEmpty()) {
                // No saved profile. The secret agent is out of scope (ADR-008), so
                // say so plainly rather than failing opaquely.
                if (done) {
                    done(false,
                        tr("No saved connection for '%1'. NetworkManager's secret agent is "
                           "not implemented; see ADR-008.")
                            .arg(ssid));
                }
                return;
            }
            QDBusMessage activate = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                QString::fromLatin1(kNmSettings),
                QString::fromLatin1(kNmSettingsConnection),
                QStringLiteral("ActivateConnection"));
            activate.setArguments({ QDBusObjectPath(QStringLiteral("/") + found),
                QDBusObjectPath(QString()), QDBusObjectPath(QString()) });
            auto *w2 = new QDBusPendingCallWatcher(bus.asyncCall(activate, 5000), this);
            connect(w2, &QDBusPendingCallWatcher::finished, this,
                [w2, done](QDBusPendingCallReply<QDBusObjectPath> r2) {
                    w2->deleteLater();
                    if (done) {
                        done(!r2.isError(), r2.error().message());
                    }
                });
        });
}

void NetworkService::disconnectWifi(std::function<void(bool)> done)
{
    if (!m_nmReady) {
        if (done) {
            done(false);
        }
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
        QString::fromLatin1(kNmPath), QString::fromLatin1(kNmInterface),
        QStringLiteral("DeactivateConnection"));
    msg.setArguments({ QDBusObjectPath(m_activeConnectionPath) });
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [w, done](QDBusPendingCallReply<> r) {
        w->deleteLater();
        if (done) {
            done(!r.isError());
        }
    });
}

} // namespace kapah