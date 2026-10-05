#include "backlight.h"

#include "logind.h"
#include "logging.h"
#include "util.h"

#include <QDir>
#include <QFile>
#include <QSocketNotifier>

#include <libudev.h>

#include <sys/inotify.h>
#include <unistd.h>

namespace kapah {

namespace {
const char *kSysfsRoot = "/sys/class/backlight";
}

BacklightService::BacklightService(QObject *parent)
    : QObject(parent)
{
}

BacklightService::~BacklightService()
{
    stop();
}

void BacklightService::start()
{
    refreshFromSysfs();


    if (m_devices.isEmpty()) {
        LOG_CAT(Backlight, "no backlight devices: the brightness module is unavailable");
        return;
    }
    m_available = true;
    Q_EMIT availableChanged(true);

    startUdev();
    startInotify();
}

void BacklightService::stop()
{
    if (m_udevMonitor != nullptr) {
        delete m_udevNotifier;
        m_udevNotifier = nullptr;
        udev_monitor_unmonitor(reinterpret_cast<udev_monitor *>(m_udevMonitor));
        m_udevMonitor = nullptr;
    }
    if (m_udev != nullptr) {
        udev_unref(reinterpret_cast<udev *>(m_udev));
        m_udev = nullptr;
    }
    if (m_inotifyFd >= 0) {
        delete m_inotifyNotifier;
        m_inotifyNotifier = nullptr;
        ::close(m_inotifyFd);
        m_inotifyFd = -1;
    }
}

QString BacklightService::writeMethod() const
{
    return m_writeMethod;
}

void BacklightService::refreshFromSysfs()
{
    QDir root(kSysfsRoot);
    if (!root.exists()) {
        if (m_available) {
            m_available = false;
            Q_EMIT availableChanged(false);
        }
        return;
    }

    m_devices.clear();
    const auto entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const QString dir = root.filePath(entry);
        BacklightDevice d;
        d.name = entry;
        d.actualBrightnessPath = dir + QStringLiteral("/actual_brightness");
        // `brightness` is the writable one; `actual_brightness` may be absent.
        if (!QFile::exists(d.actualBrightnessPath)) {
            d.actualBrightnessPath = dir + QStringLiteral("/brightness");
        }
        int v = 0;
        if (!util::readSysfsInt(dir + QStringLiteral("/brightness"), &v)) {
            continue;
        }
        d.current = v;
        if (!util::readSysfsInt(dir + QStringLiteral("/max_brightness"), &v)) {
            continue;
        }
        d.maximum = v;
        m_devices.append(d);
    }

    // Prefer the panel: largest maximum wins, because keyboard backlights report
    // a maximum of 1 or 3.
    m_primaryIndex = -1;
    int bestMax = 0;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).maximum > bestMax) {
            bestMax = m_devices.at(i).maximum;
            m_primaryIndex = i;
        }
    }

    const int newPercent = m_primaryIndex >= 0 ? m_devices.at(m_primaryIndex).percent() : 0;
    if (newPercent != m_percent) {
        m_percent = newPercent;
        Q_EMIT stateChanged();
    }

    if (m_udevMonitor == nullptr && !m_devices.isEmpty()) {
        m_writeMethod = QStringLiteral("sysfs");
        if (m_logind != nullptr && m_logind->available()) {
            m_writeMethod = QStringLiteral("logind");
        }
    }
}

void BacklightService::startUdev()
{
    udev *context = udev_new();
    if (context == nullptr) {
        return;
    }
    m_udev = context;

    udev_monitor *monitor = udev_monitor_new_from_netlink(context, "udev");
    if (monitor == nullptr) {
        udev_unref(context);
        m_udev = nullptr;
        return;
    }
    // Filter before enabling: one fd, zero wakeups for unrelated device events.
    udev_monitor_filter_add_match_subsystem_devtype(monitor, "backlight", nullptr);
    if (udev_monitor_enable_receiving(monitor) < 0) {
        LOG_WARN(Backlight, "udev backlight monitor unavailable; brightness updates rely on "
                            "the redundant inotify watch only");
        udev_monitor_unmonitor(monitor);
        udev_unref(context);
        m_udev = nullptr;
        m_udevMonitor = nullptr;
        return;
    }

    m_udevMonitor = monitor;
    m_udevNotifier = new QSocketNotifier(udev_monitor_get_fd(monitor), QSocketNotifier::Read, this);
    connect(m_udevNotifier, &QSocketNotifier::activated, this, [this, monitor] {
        while (true) {
            udev_device *device = udev_monitor_receive_device(monitor);
            if (device == nullptr) {
                break;
            }
            udev_device_unref(device);
            refreshFromSysfs();
        }
    });

    if (m_logind != nullptr && m_logind->available()) {
        m_writeMethod = QStringLiteral("logind");
    }
}

void BacklightService::startInotify()
{
    // Redundant trigger, per ADR-007. Cheap: one fd for the whole service.
    m_inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotifyFd < 0) {
        return;
    }
    QDir root(kSysfsRoot);
    const auto entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        ::inotify_add_watch(m_inotifyFd,
            QFile::encodeName(root.filePath(entry)).constData(),
            IN_MODIFY | IN_ATTRIB);
    }
    if (entries.isEmpty()) {
        ::close(m_inotifyFd);
        m_inotifyFd = -1;
        return;
    }

    m_inotifyNotifier = new QSocketNotifier(m_inotifyFd, QSocketNotifier::Read, this);
    connect(m_inotifyNotifier, &QSocketNotifier::activated, this, [this] {
        // Drain, then re-read once. Draining matters: a level-triggered notifier
        // costs a wakeup per read if we return early.
        char buf[4096];
        while (::read(m_inotifyFd, buf, sizeof(buf)) > 0) {
        }
        refreshFromSysfs();
    });
}

bool BacklightService::writeSysfs(int absoluteValue)
{
    if (m_primaryIndex < 0) {
        return false;
    }
    const QString path =
        QStringLiteral("/sys/class/backlight/") + m_devices.at(m_primaryIndex).name
        + QStringLiteral("/brightness");
    // Apply optimistically so the bar updates immediately; the uevent/inotify
    // round trip will confirm or correct it within a few milliseconds.
    m_devices[m_primaryIndex].current = absoluteValue;
    m_percent = m_devices.at(m_primaryIndex).percent();
    Q_EMIT stateChanged();
    return util::writeSysfs(path, QString::number(absoluteValue));
}

bool BacklightService::setPercent(int percent)
{
    percent = qBound(0, percent, 100);
    if (!m_available || m_primaryIndex < 0) {
        return false;
    }

    const BacklightDevice &d = m_devices.at(m_primaryIndex);
    if (d.maximum <= 0) {
        return false;
    }
    // Always write, even if the value is unchanged: a user dragging to 40% when
    // it is already 40% should still see the OSD and a confirm.
    const int absolute = qBound(0, (percent * d.maximum) / 100, d.maximum);

    if (m_writeMethod == QLatin1String("logind") && m_logind != nullptr) {
        // Optimistic update; the D-Bus reply and the uevent will reconcile.
        m_percent = percent;
        Q_EMIT stateChanged();
        m_logind->setBrightness(percent);
        Q_EMIT brightnessChangedByUser(percent);
        return true;
    }

    if (m_writeMethod == QLatin1String("sysfs")) {
        const bool ok = writeSysfs(absolute);
        if (ok) {
            m_percent = percent;
            Q_EMIT brightnessChangedByUser(percent);
        } else {
            LOG_WARN(Backlight,
                "cannot write /sys/class/backlight: needs the 'video' group or logind");
            m_writeMethod = QStringLiteral("none");
        }
        return ok;
    }

    LOG_WARN(Backlight, "no write path for the backlight; brightness changes are unavailable");
    return false;
}

} // namespace kapah
