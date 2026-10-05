// Logind (systemd-logind) client.
//
// Three distinct jobs, all of them things logind owns and nothing else does
// without root:
//
//   1. Brightness writes that do not need root:
//      org.freedesktop.login1.Session.SetBrightness ("light", 0-100).
//      Writing /sys/class/backlight/*/brightness needs the
//      "backlight" ACL group, which a desktop session usually does not have.
//   2. Idle inhibitors:
//      org.freedesktop.login1.Manager.Inhibit(what, who, why, mode) returning a
//      fd number to hold in a file descriptor. Released by closing the fd. This
//      is how "do not suspend while a video is playing" works.
//   3. Power management:
//      Suspend() / Hibernate() / PowerOff() / Reboot() on the Manager, plus
//      CanHibernate so the power menu can hide the button.
//      Also PrepareForSleep, PrepareForShutdown, LidSwitch and
//      LockSessionHint / LockSessions.
//
// Everything is a pending call with a watcher. No blocking calls, ever.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

class QDBusPendingCallWatcher;

namespace kapah {

class LogindService : public QObject {
    Q_OBJECT
public:
    enum class InhibitorMode {
        Block,         // delay the action, then let it through after our fd closes
        Ignore,        // do not delay, but log the inhibition
        Delay,         // delay, and the caller decides what to do on timeout
    };
    Q_ENUM(InhibitorMode)

    // An inhibitor is a file descriptor. Holding it *is* holding the inhibitor;
    // closing it releases. That is why this class stores an fd rather than a
    // flag.
    class Inhibitor {
    public:
        Inhibitor() = default;
        explicit Inhibitor(int fd) : m_fd(fd) {}
        ~Inhibitor() { release(); }
        Inhibitor(Inhibitor &&o) noexcept : m_fd(o.m_fd) { o.m_fd = -1; }
        Inhibitor &operator=(Inhibitor &&o) noexcept
        {
            if (this != &o) {
                release();
                m_fd = o.m_fd;
                o.m_fd = -1;
            }
            return *this;
        }
        Inhibitor(const Inhibitor &) = delete;
        Inhibitor &operator=(const Inhibitor &) = delete;

        bool held() const { return m_fd >= 0; }
        void release();

    private:
        int m_fd = -1;
    };

    explicit LogindService(QObject *parent = nullptr);
    ~LogindService() override;

    bool available() const { return m_available; }
    QString sessionPath() const { return m_sessionPath; }

    // Connects and queries the static properties. Safe to call before the event
    // loop starts.
    void start();

    // --- brightness (roadmap Phase 3.7) -----------------------------------
    // 0-100. Failures are reported through the returned bool via the callback.
    void setBrightness(int percent, std::function<void(bool)> done = {});
    int brightnessPercent() const { return m_brightness; }

    // --- power management -------------------------------------------------
    bool canSuspend() const { return m_canSuspend; }
    bool canHibernate() const { return m_canHibernate; }
    bool canPowerOff() const { return m_canPowerOff; }
    bool canReboot() const { return m_canReboot; }
    bool onBattery() const { return m_onBattery; }

    void suspend(std::function<void(bool)> done = {});
    void hibernate(std::function<void(bool)> done = {});
    void powerOff(std::function<void(bool)> done = {});
    void reboot(std::function<void(bool)> done = {});

    // Locks the screen through logind. Used as a belt-and-braces measure on lid
    // close: even if kapah-lock somehow failed to start, logind will still lock.
    void lockSession(std::function<void(bool)> done = {});

    // --- inhibitors -------------------------------------------------------
    // Takes a delay inhibitor. The returned Inhibitor must be kept alive for as
    // long as the inhibition should last; destroying it releases.
    void takeInhibitor(const QString &what, const QString &who, const QString &why,
        InhibitorMode mode, std::function<void(Inhibitor)> done);

Q_SIGNALS:
    void availableChanged(bool available);
    void capabilitiesChanged();
    void aboutToSuspend();
    void aboutToPowerOff();
    void aboutToReboot();
    void lidSwitchChanged(bool closed);
    void lockHintChanged(bool shouldLock);

private:
    void onPropertiesChanged(const QVariantMap &changed, const QVariantMap &invalidated);
    void onNameOwnerChanged(const QString &service, const QString &oldOwner, const QString &newOwner);
    void parseProperties();

    bool m_available = false;
    QString m_sessionPath;

    int m_brightness = -1;
    bool m_canSuspend = false;
    bool m_canHibernate = false;
    bool m_canPowerOff = false;
    bool m_canReboot = false;
    bool m_onBattery = false;

    QObject *m_watcher = nullptr;
    bool m_warnedNoLogind = false;
};

} // namespace kapah