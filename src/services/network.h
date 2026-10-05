// Network service: NetworkManager over D-Bus, with a netlink fallback.
//
// Owner decision (roadmap 14.4): NetworkManager is the supported stack at launch,
// with a NETLINK_ROUTE + /sys/class/net fallback so the bar still shows link
// state on a system that does not run NM. iwd and ConnMan are explicitly out of
// scope for v1 (see the roadmap's own deferral list).
//
// Event sources, all push:
//   * NM: StateChanged, PropertiesChanged, DeviceAdded/DeviceRemoved,
//     NewConnection, ConnectionUpdated. Route/scope enums from
//     NM's D-Bus API, which is stable and does not require libnm.
//   * netlink: NETLINK_ROUTE RTM_GETLINK / RTM_NEWLINK / RTM_DELLINK messages,
//     read through a QSocketNotifier. This is how `ip link` learns about
//     interfaces, and it needs no privileges.
//
// NEVER polled. The AP list and the scan are only requested while the wifi
// selector is open (roadmap 7.4), and the request is explicit.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QSocketNotifier;

namespace kapah {

enum class NetworkState {
    Unknown,
    Offline,
    Connecting,
    Connected,
};

enum class NetworkKind {
    None,
    Wired,
    Wireless,
    Bluetooth,
    Vpn,
};

struct AccessPoint {
    QString ssid;
    QString bssid;
    quint32 frequency = 0;
    quint32 signalStrength = 0; // 0-100, normalised from dBm
    int band = 0;               // 0 unknown, 1 2.4 GHz, 2 5 GHz, 3 6 GHz
    bool secured = false;
    bool active = false;
    QString path; // NM D-Bus object path, used for activation
};

struct NetworkInterface {
    QString name;
    QString typeName; // NM device type: "ethernet", "wifi", "bluetooth", ...
    NetworkKind kind = NetworkKind::None;
    NetworkState state = NetworkState::Unknown;
    bool carrier = false;   // physical link present
    bool hasAddresses = false;
    QString activeConnection; // NM connection id or name
    QString stateReason;
    // The SSID for a wifi device, for the bar's tooltip.
    QString ssid;
    int signalStrength = 0;
};

class NetworkService : public QObject {
    Q_OBJECT
public:
    explicit NetworkService(QObject *parent = nullptr);
    ~NetworkService() override;

    // `backends` comes from [services] network-backends. Empty means autodetect:
    // try NetworkManager, then netlink.
    void start(const QStringList &backends = {});
    void stop();

    bool available() const { return m_available; }
    QString backendName() const;

    // The interface the bar should show: the active one, preferring the focused
    // output's output only if the caller asks. Empty when there is nothing.
    QString primaryInterface() const { return m_primary; }
    NetworkState state() const { return m_state; }
    NetworkKind kind() const { return m_kind; }
    bool connected() const { return m_state == NetworkState::Connected; }
    int signalStrength() const { return m_signalStrength; } // 0-100, wifi only
    QString ssid() const { return m_ssid; }

    QVector<NetworkInterface> interfaces() const { return m_interfaces; }

    // --- wifi list (only meaningful while the selector is open) -------------
    bool hasWifi() const;
    QVector<AccessPoint> accessPoints() const { return m_accessPoints; }
    // Asks NM to activate an AP's saved connection, or to connect to an open one.
    // The secret agent is out of scope (ADR-008), so a secured AP with no saved
    // profile fails with a clear message.
    void activate(const QString &ssid, std::function<void(bool ok, const QString &error)> done);
    void requestScan(std::function<void()> finished);
    void disconnectWifi(std::function<void(bool)> done = {});

    // WirelessEnabled lives on the wifi device's object, so the caller supplies
    // the D-Bus path. wifiDevicePath() finds it, but only while a panel is open.
    QString wifiDevicePath();
    void setWifiEnabledOnDevice(const QString &devicePath, bool enabled,
        std::function<void(bool)> done = {});

Q_SIGNALS:
    void stateChanged();
    void availableChanged(bool available);
    void interfacesChanged();

private:
    void startNetworkManager();
    void startNetlink();
    void stopNetworkManager();

    void onNmStateChanged(quint32 newState);
    void refreshNmDevices();
    void refreshNmDevice(const QString &objectPath);
    void refreshActiveConnection();
    void subscribeToSignals();
    void readAccessPoint(const QString &apPath);
    void recomputePrimary();

    void readNetlink();
    void readSysfsInterfaces();

    bool m_available = false;
    QString m_backend;

    // NetworkManager
    bool m_nmEnabled = false;
    bool m_nmReady = false;
    QString m_nmState; // "unknown" | "asleep" | "disconnected" | "disconnected-checking"
                      // | "connecting" | "connected" | "connecting-full"
    QString m_activeConnectionPath;
    QString m_activeConnectionId;
    QString m_activeApPath;
    QString m_wifiDevicePath;
    class QObject *m_nmWatcher = nullptr;
    class QDBusPendingCallWatcher *m_scanWatcher = nullptr;
    bool m_scanInFlight = false;

    // netlink
    int m_netlinkFd = -1;
    QSocketNotifier *m_netlinkNotifier = nullptr;
    bool m_netlinkEnabled = false;

    QVector<NetworkInterface> m_interfaces;
    QVector<AccessPoint> m_accessPoints;
    QHash<QString, QString> m_wirelessEnabledByInterface;

    QString m_primary;
    NetworkState m_state = NetworkState::Unknown;
    NetworkKind m_kind = NetworkKind::None;
    QString m_ssid;
    int m_signalStrength = 0;
    QObject *m_retry = nullptr;
};

} // namespace kapah