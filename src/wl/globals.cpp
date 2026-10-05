#include "globals.h"

#include "logging.h"
#include "qtcompat.h"

#include <QGuiApplication>
#include <QScreen>
#include <QVector>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <cstring>

namespace kapah {

namespace {

// The snapshot. Written only from the registry listener on the main thread,
// which is the thread the wayland display is dispatched on.
QHash<QString, quint32> g_versions;
QHash<QString, quint32> g_ids;
// Proxies we bound ourselves and therefore have to destroy. Keyed by the
// interface name so a module can look its own proxy up.
QHash<QString, wl_registry *> g_destructors;

const QVector<ProtocolRequirement> &requirementsTable()
{
    static const QVector<ProtocolRequirement> table = {
        // interface, min version, what breaks without it
        { "zwlr_layer_shell_v1", 1, "kapah cannot draw anything; this is fatal" },
        { "wl_compositor", 1, "Qt needs it; fatal" },
        { "wl_seat", 1, "no keyboard input for the launcher" },
        { "xdg_wm_base", 1, "Qt needs it internally; fatal" },
        { "ext_session_lock_manager_v1", 1, "no lock screen" },
        { "ext_idle_notifier_v1", 1, "no idle detection; timeouts never fire" },
        { "ext_data_control_manager_v1", 1, "no clipboard history" },
        { "wlr_data_control_manager_v1", 2, "no clipboard history (wlr fallback)" },
        { "zwlr_gamma_control_manager_v1", 1, "no night light" },
        { "zwlr_output_power_manager_v1", 1, "DPMS off via output power" },
        { "zwp_linux_dmabuf_v1", 3, "shm-only rendering; slower but works" },
    };
    return table;
}

QHash<QString, wl_registry *> &destructorRegistry()
{
    static QHash<QString, wl_registry *> map;
    return map;
}

// Globals we must *not* destroy: Qt owns these proxies itself.
bool ownedByQt(const QString &interface)
{
    return interface.startsWith(QLatin1String("wl_"))
        || interface.startsWith(QLatin1String("xdg_"))
        || interface.startsWith(QLatin1String("zwp_"))
        || interface == QLatin1String("zwlr_layer_shell_v1")
        || interface == QLatin1String("zwlr_output_power_manager_v1");
}

void registryGlobal(void *data, wl_registry *registry, uint32_t id, const char *interface,
    uint32_t version)
{
    Q_UNUSED(registry)
    auto *self = static_cast<Globals *>(data);
    const QString name = QString::fromLatin1(interface);

    if (self != nullptr) {
        self->noteGlobal(id, name, version);
        self->bindSeatFromRegistry(interface, id, version, registry);
    }
    g_ids.insert(name, id);

    // Record only the highest version we saw; the compositor advertises one
    // entry per interface.
    auto it = g_versions.find(name);
    if (it == g_versions.end()) {
        g_versions.insert(name, version);
    } else if (version > it.value()) {
        it.value() = version;
    }

    // Keep a destroy listener so we do not leak the proxy. Qt-owned globals get
    // none: destroying Qt's proxy would be a use-after-free.
    if (!ownedByQt(name)) {
        destructorRegistry().insert(name, registry);
    }
}

void registryGlobalRemove(void *data, wl_registry *registry, uint32_t id)
{
    Q_UNUSED(data)
    Q_UNUSED(registry)
    Q_UNUSED(id)
    // Removing a global does not remove a capability we already bound: the
    // protocol stays alive until the object is destroyed. Recording that here
    // would produce false "unavailable" states during compositor shutdown.
}

const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

Globals *g_instance = nullptr;

void Globals::bindSeatFromRegistry(const char *interface, uint32_t name, uint32_t version,
    wl_registry *registry)
{
    if (m_seat != nullptr || std::strcmp(interface, "wl_seat") != 0) {
        return;
    }
    m_seat = static_cast<wl_seat *>(
        wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7));
}


} // namespace

void Globals::noteGlobal(quint32 id, const QString &interface, quint32 version)
{
    // The version Qt itself bound is authoritative for Qt-owned protocols: Qt
    // already made its own bind request and we must not exceed its version.
    if (interface == QLatin1String("zwlr_layer_shell_v1") && version > 4) {
        // Our vendored XML binds version 4. Binding higher would make the
        // generated code call requests that niri implements but we do not.
        version = 4;
    }
    LOG_CATF(Wayland, "global: %s v%u (id %u)", qPrintable(interface), version, id);
}

const QVector<ProtocolRequirement> &protocolRequirements()
{
    return requirementsTable();
}

QHash<QString, quint32> globalVersions()
{
    return g_versions;
}

quint32 globalId(const QString &interface)
{
    return g_ids.value(interface, 0);
}

bool hasGlobal(const QString &interface, quint32 minimumVersion)
{
    const auto it = g_versions.constFind(interface);
    if (it == g_versions.constEnd()) {
        return false;
    }
    return it.value() >= minimumVersion;
}

QStringList outputNames()
{
    QStringList names;
    const auto screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        names.append(screen->name());
    }
    names.sort();
    return names;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

Globals::Globals(QObject *parent)
    : QObject(parent)
{
    g_instance = this;
}

Globals::~Globals()
{
    // Destroy the proxies we own. Qt's own globals are left alone.
    auto &proxies = destructorRegistry();
    for (auto it = proxies.begin(); it != proxies.end(); ++it) {
        if (it.value() != nullptr) {
            wl_registry_destroy(it.value());
        }
    }
    proxies.clear();
    g_versions.clear();
    g_ids.clear();
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

void Globals::start()
{
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr || integration->display() == nullptr) {
        LOG_ERR(Wayland, "Globals::start() called before the wayland display existed");
        return;
    }
    connectRegistry();
    refresh();
    logCapabilities();
}

void Globals::refresh()
{
    readRegistry();
    Q_EMIT outputsChanged();
}

void Globals::connectRegistry()
{
    if (m_registry != nullptr) {
        return;
    }
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr || integration->display() == nullptr) {
        return;
    }
    wl_display *display = integration->display()->display();
    m_registry = wl_display_get_registry(display);
    if (m_registry == nullptr) {
        LOG_ERR(Wayland, "wl_display_get_registry returned null");
        return;
    }
    wl_registry_add_listener(m_registry, &kRegistryListener, this);
    // One round trip so the first snapshot is complete before we create any
    // surface. This happens twice per process (startup, and after a hotplug) and
    // is a blocking call by design; see docs/ARCHITECTURE.md.
    wl_display_roundtrip(display);
}

void Globals::readRegistry()
{
    // The listener already filled g_versions during connectRegistry()'s round
    // trip. Refresh() only needs to notice hotplugged outputs, which Qt already
    // reported through screenAdded/screenRemoved.
    for (const ProtocolRequirement &req : protocolRequirements()) {
        const QString name = QString::fromLatin1(req.interface);
        if (g_versions.contains(name)) {
            continue;
        }
        LOG_WARNF(Wayland, "compositor does not advertise %s: %s", req.interface,
            req.consequence);
    }

    if (!m_readyEmitted) {
        m_readyEmitted = true;
        Q_EMIT ready();
    }
}

int Globals::trackedGlobals() const
{
    return static_cast<int>(g_versions.size());
}

void Globals::logCapabilities() const
{
    LOG_INFO(Wayland, "---- wayland capabilities ----");
    for (const ProtocolRequirement &req : protocolRequirements()) {
        const QString name = QString::fromLatin1(req.interface);
        const auto it = g_versions.constFind(name);
        if (it == g_versions.constEnd()) {
            LOG_INFOF(Wayland, "  %-36s absent   (%s)", req.interface, req.consequence);
        } else {
            LOG_INFOF(Wayland, "  %-36s v%u (need v%u)", req.interface, it.value(),
                req.minimumVersion);
        }
    }
    LOG_INFOF(Wayland, "  outputs: %s", qPrintable(outputNames().join(QStringLiteral(", "))));
}

} // namespace kapah