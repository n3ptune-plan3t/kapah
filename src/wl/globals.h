// Wayland global registry: capability detection, nothing else.
//
// Roadmap section 6: "Agents must check the current niri release notes for which
// protocols are implemented; never assume support. Add runtime capability
// detection and degrade features cleanly."
//
// So the very first thing kapah does after the display connects is snapshot the
// globals we care about. Every feature that needs a protocol asks
// globalVersions() first and turns itself off when the answer is zero.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

struct wl_registry;
struct wl_seat;

namespace kapah {

// Protocols we look for, and the lowest version we can work with.
struct ProtocolRequirement {
    const char *interface;
    quint32 minimumVersion;
    // What breaks if it is missing.
    const char *consequence;
};

// The full list, in the order we report them. Kept here rather than scattered so
// that docs/PROTOCOLS.md and `kapahctl status` read from one source.
const QVector<ProtocolRequirement> &protocolRequirements();

// Snapshot of interface name -> advertised version for every global we track.
// Empty until the registry has been read once.
QHash<QString, quint32> globalVersions();

// Wayland global name ids, needed to bind a protocol Qt does not know about
// (ext-session-lock, ext-idle-notify, data-control, gamma-control). Returns 0
// when the interface is not present, which wl_registry_bind treats as invalid.
quint32 globalId(const QString &interface);

bool hasGlobal(const QString &interface, quint32 minimumVersion = 1);

    // True for the wl_output-backed screens, keyed by connector name.
    QStringList outputNames();

    // The first wl_seat, bound once at startup. ext-idle-notify and the
    // launcher's keyboard grab both need it. Null when the compositor has
    // no seat, which for a laptop does not happen.
    wl_seat *seat() const { return m_seat; }
    void bindSeatFromRegistry(const char *interface, uint32_t name, uint32_t version,
        wl_registry *registry);

class Globals : public QObject {
    Q_OBJECT
public:
    explicit Globals(QObject *parent = nullptr);
    ~Globals() override;

    // Installs the wl_registry listener. Call once, after the QGuiApplication
    // exists and before any protocol is bound.
    void start();

    // Re-reads the registry. Called after screenAdded/screenRemoved so that a
    // hotplugged output's globals are visible immediately.
    void refresh();

    // Number of globals currently tracked; used by tests.
    int trackedGlobals() const;

    // Prints the capability table to the log at Info level. Used by
    // `kapahctl status` and by the integration tests.
    void logCapabilities() const;

Q_SIGNALS:
    // Fired once the first snapshot is complete. Every service waits for this so
    // that "protocol not supported" is decided before any surface is created.
    void ready();
    // Fired on hotplug, so components can create/destroy per-output surfaces.
    void outputsChanged();

private:
    // wl_registry listener callback; records one interface's version.
    void noteGlobal(quint32 id, const QString &interface, quint32 version);

    wl_seat *m_seat = nullptr;

    void connectRegistry();
    void readRegistry();

    wl_registry *m_registry = nullptr;
    bool m_readyEmitted = false;
};

} // namespace kapah