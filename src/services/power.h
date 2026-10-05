// Power service: battery level, charging state, time remaining.
//
// Two backends, both event-driven (roadmap Phase 3.4):
//
//   1. UPower over D-Bus (org.freedesktop.UPower). Preferred because UPower
//      already watches the battery and knows the discharge curve, so
//      TimeToEmpty is meaningful. Signals we use:
//        PropertiesChanged on /org/freedesktop/UPower/devices/... and .../displayDevice
//        Receiver / Sender name watching, so a UPower restart is handled.
//
//   2. udev monitor on the power_supply subsystem, with /sys/class/power_supply
//      read *only on the event*. This exists specifically so the service does not
//      have to poll: `capacity` and `status` in sysfs do not generate inotify
//      events reliably, but the kernel does emit uevents on charge threshold
//      crossings.
//
// Neither backend polls. If both are missing the service reports
// available()==false and the bar draws a "battery unavailable" pill rather than
// nothing at all.

#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace kapah {

struct BatteryInfo {
    QString name;      // "BAT0"
    QString status;    // "Charging" | "Discharging" | "Full" | "Not charging" | "Unknown"
    int percent = -1;  // 0-100, -1 unknown
    int minutesRemaining = -1; // -1 unknown
    QString voltageMv;
    QString energyFullDesign;
    QString energyFull;
    QString energyNow;
    QString technology;
    bool present = false;

    bool charging() const { return status == QLatin1String("Charging"); }
    bool discharging() const { return status == QLatin1String("Discharging"); }
    bool low() const { return percent >= 0 && percent <= 20 && !charging(); }
    bool critical() const { return percent >= 0 && percent <= 5 && !charging(); }
};

class PowerService : public QObject {
    Q_OBJECT
public:
    explicit PowerService(QObject *parent = nullptr);
    ~PowerService() override;

    void start(bool useUpower, bool useUdev);

    bool available() const { return m_available; }
    QString backendName() const;

    // The "display device" battery: aggregate of all packs. Empty when there is
    // no battery at all (a desktop).
    const BatteryInfo &battery() const { return m_battery; }
    bool hasBattery() const { return m_battery.present; }
    bool isOnBattery() const { return m_onBattery; }

    // Packs that are present but not part of the aggregate (a second battery in
    // a dock). The power menu lists them.
    const QVector<BatteryInfo> &extraBatteries() const { return m_extras; }

    // Threshold crossings the UI turns into a notification. The service tracks
    // them so a notification fires exactly once per discharge (roadmap Phase 3.4:
    // "Low/critical thresholds trigger a notification once").
    bool takeLowNotification();
    bool takeCriticalNotification();

Q_SIGNALS:
    void stateChanged();
    void availableChanged(bool available);
    void onBatteryChanged(bool onBattery);

private:
    void startUpower();
    void stopUpower();
    void startUdev();
    void refreshFromSysfs();
    void setBattery(const BatteryInfo &info);
    void recomputeAggregate();

    bool m_available = false;
    BatteryInfo m_battery;
    QVector<BatteryInfo> m_packs;
    QVector<BatteryInfo> m_extras;
    bool m_onBattery = false;

    // Threshold latches.
    bool m_lowFired = false;
    bool m_criticalFired = false;
    bool m_lowPending = false;
    bool m_criticalPending = false;

    // Backend state.
    bool m_useUpower = true;
    bool m_useUdev = true;
    bool m_upowerActive = false;
    void *m_udev = nullptr;
    void *m_udevMonitor = nullptr;
    int m_udevFd = -1;
    class QSocketNotifier *m_udevNotifier = nullptr;

    QString m_upowerDisplayDevice;
};

} // namespace kapah