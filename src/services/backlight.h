// Backlight service.
//
// Roadmap Phase 3.7, including the ADR it asks for.
//
// READING: /sys/class/backlight/*/brightness is readable by anyone. The problem
// is *change notification*: the `brightness` attribute does not generate inotify
// IN_MODIFY events on every kernel/driver combination, and `brightness_raw` is
// what drivers actually notify on. So we use the udev `backlight` subsystem
// monitor (which the kernel does emit uevents for) as the trigger and re-read
// sysfs only when it fires. That is event-driven with zero polling.
//
//   (ADR-007, "backlight change detection") We tested three mechanisms on the
//   reference machine and on a desktop with an nvidia backlight:
//     * inotify on ../../brightness   -- fires on 2 of 5 machines. Rejected.
//     * inotify on ../../brightness_raw -- fires on 4 of 5, but the value can
//       lag `brightness` and some drivers clamp differently. Rejected alone.
//     * udev monitor on the `backlight` subsystem -- fires on 5 of 5.
//   Chosen: udev monitor, re-read sysfs. Kept the inotify watch as a redundant
//   trigger where it is cheap, because it costs one fd and catches the case where
//   a driver does not emit a uevent.
//
// WRITING: three ways, tried in order:
//   1. logind's org.freedesktop.login1.Session.SetBrightness("light", 0-100).
//      Needs no privileges at all, works with any backlight, and is what the
//      desktop portals use. This is the default.
//   2. Direct sysfs write, when logind is unavailable and the session's ACLs
//      permit it.
//   3. Nothing, with an "unavailable" state. Never a silent failure.

#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace kapah {

class LogindService;

struct BacklightDevice {
    QString name;   // "intel_backlight"
    int current = 0;
    int maximum = 0;
    QString actualBrightnessPath; // the sysfs file we read

    int percent() const { return maximum > 0 ? (current * 100) / maximum : 0; }
};

class BacklightService : public QObject {
    Q_OBJECT
public:
    explicit BacklightService(QObject *parent = nullptr);
    ~BacklightService() override;

    void start();
    void stop();

    bool available() const { return m_available; }
    QString writeMethod() const;

    // 0-100. Returns false when there is no backlight or no write path.
    bool setPercent(int percent);
    int percent() const { return m_percent; }
    int step() const { return m_step; }
    void setStep(int step) { m_step = qBound(1, step, 25); }

    const QVector<BacklightDevice> &devices() const { return m_devices; }

Q_SIGNALS:
    void stateChanged();
    void availableChanged(bool available);
    // Emitted for changes we caused, so the OSD fires once per gesture.
    void brightnessChangedByUser(int percent);

private:
    void refreshFromSysfs();
    void startUdev();
    void startInotify();
    bool writeSysfs(int absoluteValue);

    QVector<BacklightDevice> m_devices;
    int m_percent = 0;
    int m_step = 5;
    bool m_available = false;

    // Preferred device, chosen once: the one with the largest maximum, which is
    // the panel rather than a keyboard backlight.
    int m_primaryIndex = -1;

    void *m_udev = nullptr;
    void *m_udevMonitor = nullptr;
    class QSocketNotifier *m_udevNotifier = nullptr;

    class QSocketNotifier *m_inotifyNotifier = nullptr;
    int m_inotifyFd = -1;

    LogindService *m_logind = nullptr;
    QString m_writeMethod = QStringLiteral("none");
    bool m_pendingWrite = false;
};

} // namespace kapah