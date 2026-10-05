#include "idlenotify.h"

#include "globals.h"
#include "logging.h"
#include "qtcompat.h"

#include "ext-idle-notify-v1-client-protocol.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace kapah {

namespace {

void onIdledThunk(void *data, ext_idle_notification_v1 *notification)
{
    Q_UNUSED(notification)
    if (auto *self = static_cast<IdleNotifier *>(data)) {
        self->onIdled();
    }
}

void onResumedThunk(void *data, ext_idle_notification_v1 *notification)
{
    Q_UNUSED(notification)
    if (auto *self = static_cast<IdleNotifier *>(data)) {
        self->onResumed();
    }
}

// ext_idle_notification_v1 declares exactly two events, idled then resumed, in
// that order. The struct literal must stay in XML order or every callback fires
// the wrong slot -- see ext-idle-notify-v1-client-protocol.h.
const ext_idle_notification_v1_listener kIdleListener = { onIdledThunk, onResumedThunk };

} // namespace

IdleNotifier::IdleNotifier(QObject *parent)
    : QObject(parent)
{
}

IdleNotifier::~IdleNotifier()
{
    stop();
}

bool IdleNotifier::start()
{
    if (m_available) {
        return true;
    }

    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr || integration->display() == nullptr) {
        LOG_ERR(Wayland, "IdleNotifier::start() before the display existed");
        return false;
    }

    const QString iface = QStringLiteral("ext_idle_notifier_v1");
    const auto it = globalVersions().constFind(iface);
    if (it == globalVersions().end() || it.value() < 1) {
        LOG_WARN(Wayland,
            "ext_idle_notifier_v1 not advertised: idle detection, dimming, auto-lock and "
            "DPMS are disabled. See docs/PROTOCOLS.md.");
        return false;
    }
    // We only use the two requests below; v2 adds get_input_idle_notification.
    m_version = qMin<quint32>(it.value(), 2);

    const quint32 name = globalId(iface);
    if (name == 0) {
        LOG_WARN(Wayland, "ext_idle_notifier_v1 vanished between the snapshot and the bind");
        return false;
    }

    wl_display *display = integration->display()->display();

    // Qt does not know this protocol, so bind it ourselves. Our own short-lived
    // registry proxy; the global id stays valid for the connection's lifetime.
    wl_registry *registry = wl_display_get_registry(display);
    if (registry == nullptr) {
        return false;
    }
    // No second round trip: Globals::connectRegistry() already completed one, so
    // globalId() is populated.
    m_notifier = static_cast<ext_idle_notifier_v1 *>(
        wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, m_version));
    wl_registry_destroy(registry);

    if (m_notifier == nullptr) {
        LOG_WARN(Wayland, "wl_registry_bind(ext_idle_notifier_v1) failed");
        return false;
    }

    if (m_version >= 2) {
        // "Input idle" is what a user means by "the machine went idle": it
        // ignores, for example, a lid-switch event or a key held by an
        // automation tool that is not really a user.
        m_notification = ext_idle_notifier_v1_get_input_idle_notification(
            m_notifier, m_timeoutMs, m_seat);
        m_inputIdle = true;
    } else {
        m_notification = ext_idle_notifier_v1_get_idle_notification(
            m_notifier, m_timeoutMs, m_seat);
        m_inputIdle = false;
    }

    if (m_notification == nullptr) {
        LOG_WARN(Wayland, "ext_idle_notifier_v1 gave us no notification object");
        ext_idle_notifier_v1_destroy(m_notifier);
        m_notifier = nullptr;
        return false;
    }

    ext_idle_notification_v1_add_listener(m_notification, &kIdleListener, this);
    m_available = true;

    LOG_INFOF(Wayland, "ext_idle_notifier_v1 v%u bound (%s)", m_version,
        m_inputIdle ? "input idle" : "idle");
    return true;
}

void IdleNotifier::stop()
{
    if (m_notification != nullptr) {
        ext_idle_notification_v1_destroy(m_notification);
        m_notification = nullptr;
    }
    if (m_notifier != nullptr) {
        ext_idle_notifier_v1_destroy(m_notifier);
        m_notifier = nullptr;
    }
    m_available = false;
    m_idle = false;
}

void IdleNotifier::onIdled()
{
    if (m_idle) {
        return;
    }
    m_idle = true;
    LOG_CAT(Wayland, "session went idle");
    Q_EMIT idled();
}

void IdleNotifier::onResumed()
{
    if (!m_idle) {
        return;
    }
    m_idle = false;
    LOG_CAT(Wayland, "session resumed");
    Q_EMIT resumed();
}

} // namespace kapah