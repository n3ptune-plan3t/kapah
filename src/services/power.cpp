#include "power.h"

#include "logging.h"
#include "util.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QSocketNotifier>

#include <libudev.h>

#include <poll.h>

namespace kapah {

namespace {

const char *kUpowerService = "org.freedesktop.UPower";
const char *kUpowerPath = "/org/freedesktop/UPower";
const char *kUpowerInterface = "org.freedesktop.UPower";
const char *kDeviceInterface = "org.freedesktop.UPower.Device";
const char *kPropsInterface = "org.freedesktop.DBus.Properties";
const char *kSysfsRoot = "/sys/class/power_supply";

// Reads one attribute of a power_supply pack. Returns false when the pack does
// not have it, which is the normal case for most attributes.
QString readAttr(const QString &dir, const char *name)
{
    QString v;
    if (!util::readSysfs(dir + QLatin1Char('/') + QLatin1String(name), &v)) {
        return {};
    }
    return v;
}

bool packIsBattery(const QString &dir)
{
    const QString type = readAttr(dir, "type");
    return type == QLatin1String("Battery");
}

} // namespace

PowerService::PowerService(QObject *parent)
    : QObject(parent)
{
}

PowerService::~PowerService()
{
    if (m_udevMonitor != nullptr) {
        udev_monitor_unmonitor(reinterpret_cast<udev_monitor *>(m_udevMonitor));
    }
    if (m_udev != nullptr) {
        udev_unref(reinterpret_cast<udev *>(m_udev));
    }
}

void PowerService::start(bool useUpower, bool useUdev)
{
    m_useUpower = useUpower;
    m_useUdev = useUdev;

    // udev first: it gives us an answer immediately on a laptop, and UPower may
    // take a second to come up. sysfs is read once here and then only on events.
    refreshFromSysfs();

    if (m_useUdev) {
        startUdev();
    }
    if (m_useUpower) {
        startUpower();
    }
}

QString PowerService::backendName() const
{
    if (m_upowerActive) {
        return QStringLiteral("upower");
    }
    if (m_udevMonitor != nullptr) {
        return QStringLiteral("udev");
    }
    return QStringLiteral("none");
}

// ---------------------------------------------------------------------------
// sysfs
// ---------------------------------------------------------------------------

void PowerService::refreshFromSysfs()
{
    QDir root(kSysfsRoot);
    if (!root.exists()) {
        setBattery(BatteryInfo {});
        return;
    }

    QVector<BatteryInfo> packs;
    const auto entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const QString dir = root.filePath(entry);
        if (!packIsBattery(dir)) {
            continue;
        }
        BatteryInfo b;
        b.name = entry;
        b.present = true;
        b.status = readAttr(dir, "status");
        if (b.status.isEmpty()) {
            b.status = QStringLiteral("Unknown");
        }
        bool ok = false;
        const QString cap = readAttr(dir, "capacity");
        b.percent = cap.toInt(&ok);
        if (!ok) {
            b.percent = -1;
        }
        b.technology = readAttr(dir, "technology");
        b.voltageMv = readAttr(dir, "voltage_now");
        b.energyFullDesign = readAttr(dir, "energy_full_design");
        b.energyFull = readAttr(dir, "energy_full");
        b.energyNow = readAttr(dir, "energy_now");

        // Discharging packs that have never been calibrated report absurd values
        // (0 minutes, or 4 billion). Treat those as unknown.
        QString remaining;
        switch (b.charging() ? 1 : 0) {
        case 0:
            remaining = readAttr(dir, "time_to_empty_now");
            break;
        default:
            remaining = readAttr(dir, "time_to_full_now");
            break;
        }
        const int secs = remaining.toInt(&ok);
        if (ok && secs > 0 && secs < 60 * 60 * 24) {
            b.minutesRemaining = secs / 60;
        }

        packs.append(b);
    }

    m_packs = packs;
    recomputeAggregate();
}

void PowerService::recomputeAggregate()
{
    if (m_packs.isEmpty()) {
        setBattery(BatteryInfo {});
        m_extras.clear();
        return;
    }

    // The aggregate is the sum, which is what UPower's DisplayDevice reports and
    // what a user with two packs expects to see.
    BatteryInfo agg;
    agg.present = true;
    agg.name = QStringLiteral("aggregate");
    int totalPercentSum = 0;
    int packsWithPercent = 0;
    int minutesSum = 0;
    int packsWithMinutes = 0;

    bool anyCharging = false;
    bool anyDischarging = false;
    bool anyFull = false;

    for (const BatteryInfo &p : m_packs) {
        if (p.percent >= 0) {
            totalPercentSum += p.percent;
            ++packsWithPercent;
        }
        if (p.minutesRemaining >= 0) {
            minutesSum += p.minutesRemaining;
            ++packsWithMinutes;
        }
        anyCharging = anyCharging || p.charging();
        anyDischarging = anyDischarging || p.discharging();
        anyFull = anyFull || p.status == QLatin1String("Full");

        // Prefer any charging pack's status for the aggregate, since that is what
        // the machine is doing overall.
        if (p.charging()) {
            agg.status = QStringLiteral("Charging");
            if (!p.energyNow.isEmpty()) {
                agg.energyNow = p.energyNow;
            }
            if (!p.energyFull.isEmpty()) {
                agg.energyFull = p.energyFull;
            }
        }
    }

    if (agg.status.isEmpty()) {
        if (anyDischarging) {
            agg.status = QStringLiteral("Discharging");
        } else if (anyFull) {
            agg.status = QStringLiteral("Full");
        } else {
            agg.status = QStringLiteral("Not charging");
        }
    }

    agg.percent = packsWithPercent > 0 ? totalPercentSum / packsWithPercent : -1;
    agg.minutesRemaining = packsWithMinutes > 0 ? minutesSum / packsWithMinutes : -1;
    if (!agg.energyNow.isEmpty()) {
        agg.technology = QStringLiteral("Energy-based");
    }

    m_extras.clear();
    for (int i = 1; i < m_packs.size(); ++i) {
        m_extras.append(m_packs.at(i));
    }

    const bool wasOnBattery = m_onBattery;
    m_onBattery = agg.discharging();
    setBattery(agg);

    if (wasOnBattery != m_onBattery) {
        Q_EMIT onBatteryChanged(m_onBattery);
    }
}

void PowerService::setBattery(const BatteryInfo &info)
{
    const bool hadBattery = m_battery.present;
    m_battery = info;

    if (info.present) {
        m_available = true;
        // Threshold latches reset only when we cross back *up*, so a battery that
        // oscillates around 20% does not produce a stream of notifications.
        if (info.percent > 25) {
            m_lowFired = false;
        }
        if (info.percent > 10) {
            m_criticalFired = false;
        }
        if (!m_lowFired && info.low()) {
            m_lowFired = true;
            m_lowPending = true;
        }
        if (!m_criticalFired && info.critical()) {
            m_criticalFired = true;
            m_criticalPending = true;
        }
    }

    if (hadBattery != info.present) {
        Q_EMIT availableChanged(info.present);
    }
    Q_EMIT stateChanged();
}

bool PowerService::takeLowNotification()
{
    if (!m_lowPending) {
        return false;
    }
    m_lowPending = false;
    return true;
}

bool PowerService::takeCriticalNotification()
{
    if (!m_criticalPending) {
        return false;
    }
    m_criticalPending = false;
    return true;
}

// ---------------------------------------------------------------------------
// udev
// ---------------------------------------------------------------------------

void PowerService::startUdev()
{
    udev *context = udev_new();
    if (context == nullptr) {
        LOG_WARN(Power, "udev_new() failed: battery updates rely on UPower only");
        return;
    }
    m_udev = context;

    udev_monitor *monitor = udev_monitor_new_from_netlink(context, "udev");
    if (monitor == nullptr) {
        LOG_WARN(Power, "udev_monitor_new_from_netlink failed");
        udev_unref(context);
        m_udev = nullptr;
        return;
    }

    // Filter before enabling so the kernel does not wake us for every device on
    // the bus. This is the single most important line in this file for the
    // roadmap's wakeup budget.
    if (udev_monitor_filter_add_match_subsystem_devtype(
            monitor, "power_supply", nullptr) < 0) {
        LOG_WARN(Power, "udev: could not filter on power_supply; updates may be noisy");
    }
    if (udev_monitor_enable_receiving(monitor) < 0) {
        LOG_WARN(Power, "udev: cannot enable the monitor (needs CAP_NET_ADMIN? no: needs "
                        "udev group membership)");
        udev_monitor_unmonitor(monitor);
        udev_unref(context);
        m_udev = nullptr;
        return;
    }

    m_udevMonitor = monitor;
    m_udevFd = udev_monitor_get_fd(monitor);

    m_udevNotifier = new QSocketNotifier(m_udevFd, QSocketNotifier::Read, this);
    connect(m_udevNotifier, &QSocketNotifier::activated, this, [this, monitor] {
        // Drain the whole queue: level-triggered notifier, and one read per event
        // would cost one wakeup each.
        while (true) {
            udev_device *device = udev_monitor_receive_device(monitor);
            if (device == nullptr) {
                break;
            }
            const char *action = udev_device_get_action(device);
            // "change" is the charge-level event; "add"/"remove" are hotplug.
            if (action != nullptr
                && (qstrcmp(action, "change") == 0 || qstrcmp(action, "add") == 0
                    || qstrcmp(action, "remove") == 0)) {
                refreshFromSysfs();
            }
            udev_device_unref(device);
        }
    });

    LOG_INFO(Power, "udev power_supply monitor active");
}

// ---------------------------------------------------------------------------
// UPower
// ---------------------------------------------------------------------------

void PowerService::startUpower()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }

    auto *watcher = new QDBusServiceWatcher(
        QString::fromLatin1(kUpowerService), bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
        startUpower();
    });
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        stopUpower();
        // Fall back to whatever sysfs said.
        refreshFromSysfs();
    });

    // UPower's DisplayDevice is the aggregate battery; that is all we need from
    // UPower, so we do not enumerate devices at all.
    m_upowerDisplayDevice = QStringLiteral("/org/freedesktop/UPower/devices/display_device");

    auto *objWatcher = new QDBusObjectWatcher(m_upowerDisplayDevice, bus,
        bus.interface(), this);
    connect(objWatcher, &QDBusObjectWatcher::propertiesChanged, this,
        [this](const QString &, const QVariantMap &changed, const QVariantMap &) {
            BatteryInfo info = m_battery;
            if (changed.contains(QStringLiteral("Percentage"))) {
                info.percent = changed.value(QStringLiteral("Percentage")).toInt();
            }
            if (changed.contains(QStringLiteral("State"))) {
                // UPower's State enum: 0 unknown, 1 charging, 2 discharging,
                // 3 not charging, 4 fully charged.
                switch (changed.value(QStringLiteral("State")).toUInt()) {
                case 1:
                    info.status = QStringLiteral("Charging");
                    break;
                case 2:
                    info.status = QStringLiteral("Discharging");
                    break;
                case 3:
                    info.status = QStringLiteral("Not charging");
                    break;
                case 4:
                    info.status = QStringLiteral("Full");
                    break;
                default:
                    info.status = QStringLiteral("Unknown");
                    break;
                }
            }
            if (changed.contains(QStringLiteral("TimeToEmpty"))) {
                const qint64 secs = changed.value(QStringLiteral("TimeToEmpty")).toLongLong();
                // 0 means "not supported"; negative 1 means "unknown"; a value
                // above a day means "not supported".
                info.minutesRemaining = (secs > 0 && secs < 60 * 60 * 24) ? secs / 60 : -1;
            }
            if (info.percent >= 0) {
                info.present = true;
            }
            const bool wasOnBattery = m_onBattery;
            m_onBattery = info.status == QLatin1String("Discharging");
            setBattery(info);
            if (wasOnBattery != m_onBattery) {
                Q_EMIT onBatteryChanged(m_onBattery);
            }
        });

    // Initial property read.
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kUpowerService),
        m_upowerDisplayDevice, QString::fromLatin1(kPropsInterface), QStringLiteral("GetAll"));
    msg.setArguments({ QString::fromLatin1(kDeviceInterface) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w](
                                                         QDBusPendingCallReply<QVariantMap> r) {
        w->deleteLater();
        if (r.isError()) {
            // UPower not running: not an error, we have udev/sysfs.
            LOG_CAT(Power, "UPower unavailable: %s", qPrintable(r.error().message()));
            return;
        }
        m_upowerActive = true;

        const QVariantMap props = r.value();
        BatteryInfo info;
        info.present = props.contains(QStringLiteral("Percentage"));
        info.percent = props.value(QStringLiteral("Percentage"), -1).toInt();
        switch (props.value(QStringLiteral("State"), 0u).toUInt()) {
        case 1:
            info.status = QStringLiteral("Charging");
            break;
        case 2:
            info.status = QStringLiteral("Discharging");
            break;
        case 3:
            info.status = QStringLiteral("Not charging");
            break;
        case 4:
            info.status = QStringLiteral("Full");
            break;
        default:
            info.status = QStringLiteral("Unknown");
            break;
        }
        const qint64 tte = props.value(QStringLiteral("TimeToEmpty"), -1).toLongLong();
        info.minutesRemaining = (tte > 0 && tte < 60 * 60 * 24) ? tte / 60 : -1;
        const bool wasOnBattery = m_onBattery;
        m_onBattery = info.status == QLatin1String("Discharging");
        setBattery(info);
        if (wasOnBattery != m_onBattery) {
            Q_EMIT onBatteryChanged(m_onBattery);
        }
        Q_EMIT availableChanged(info.present);
    });
}

void PowerService::stopUpower()
{
    m_upowerActive = false;
}

} // namespace kapah