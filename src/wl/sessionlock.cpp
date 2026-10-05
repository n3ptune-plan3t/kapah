#include "sessionlock.h"

#include "globals.h"
#include "logging.h"
#include "qtcompat.h"

#include "ext-session-lock-v1-client-protocol.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace kapah {

namespace {

const ext_session_lock_v1_listener kLockListener = {
    &SessionLock::handleLocked,
    &SessionLock::handleFinished,
};

const ext_session_lock_surface_v1_listener kSurfaceListener = {
    &LockSurface::handleConfigure,
};

} // namespace

// ---------------------------------------------------------------------------
// SessionLock
// ---------------------------------------------------------------------------

SessionLock::SessionLock(QObject *parent)
    : QObject(parent)
{
}

SessionLock::~SessionLock()
{
    // Deliberately does NOT unlock. If kapah-lock is torn down without an
    // explicit unlock (normal exit after a successful authentication), the
    // surfaces are already destroyed and the lock released by unlockAndDestroy().
    // Any other teardown path leaves the session locked, which is correct.
    //
    // We only free our protocol objects here so the compositor does not leak.
    const auto surfaces = m_surfaces;
    m_surfaces.clear();
    for (LockSurface *s : surfaces) {
        delete s;
    }

    if (m_lock != nullptr) {
        ext_session_lock_v1_destroy(m_lock);
        m_lock = nullptr;
    }
    if (m_manager != nullptr) {
        ext_session_lock_manager_v1_destroy(m_manager);
        m_manager = nullptr;
    }
}

bool SessionLock::begin()
{
    if (m_manager != nullptr) {
        return true;
    }

    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr || integration->display() == nullptr) {
        LOG_ERR(Lock, "SessionLock::begin() before the display existed");
        return false;
    }

    const QString iface = QStringLiteral("ext_session_lock_manager_v1");
    const auto it = globalVersions().constFind(iface);
    if (it == globalVersions().end() || it.value() < 1) {
        LOG_ERR(Lock,
            "ext_session_lock_manager_v1 not advertised. niri 25.11+ implements it; if you "
            "are on an older niri there is no way to lock safely and kapah-lock will "
            "refuse to pretend otherwise. See docs/PROTOCOLS.md.");
        return false;
    }

    const quint32 name = globalId(iface);
    if (name == 0) {
        LOG_ERR(Lock, "ext_session_lock_manager_v1 vanished between snapshot and bind");
        return false;
    }

    wl_display *display = integration->display()->display();
    wl_registry *registry = wl_display_get_registry(display);
    if (registry == nullptr) {
        return false;
    }
    m_manager = static_cast<ext_session_lock_manager_v1 *>(
        wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
    wl_registry_destroy(registry);

    if (m_manager == nullptr) {
        LOG_ERR(Lock, "wl_registry_bind(ext_session_lock_manager_v1) failed");
        return false;
    }

    m_lock = ext_session_lock_manager_v1_lock(m_manager);
    if (m_lock == nullptr) {
        LOG_ERR(Lock, "ext_session_lock_manager_v1.lock() returned null");
        ext_session_lock_manager_v1_destroy(m_manager);
        m_manager = nullptr;
        return false;
    }

    ext_session_lock_v1_add_listener(m_lock, &kLockListener, this);

    LOG_INFO(Lock, "session lock requested; waiting for surfaces to be configured");
    return true;
}

LockSurface *SessionLock::createSurface(wl_surface *surface, wl_output *output)
{
    if (m_lock == nullptr || surface == nullptr) {
        LOG_ERR(Lock, "createSurface before begin()");
        return nullptr;
    }
    auto *lockSurface = new LockSurface(m_lock, surface, output, this);
    if (lockSurface->m_lockSurface == nullptr) {
        delete lockSurface;
        return nullptr;
    }
    m_surfaces.insert(lockSurface);
    Q_EMIT surfaceAdded(lockSurface);
    return lockSurface;
}

void SessionLock::destroySurface(LockSurface *surface)
{
    if (surface == nullptr || !m_surfaces.contains(surface)) {
        return;
    }
    m_surfaces.remove(surface);
    delete surface;
}

bool SessionLock::allSurfacesAcknowledged() const
{
    if (m_surfaces.isEmpty()) {
        // A lock with no surfaces would show the desktop. Never report ready.
        return false;
    }
    for (LockSurface *s : m_surfaces) {
        if (!s->acknowledged()) {
            return false;
        }
    }
    return true;
}

void SessionLock::unlockAndDestroy()
{
    // -------------------------------------------------------------------------
    // SECURITY: the ONLY call site is LockView's authentication success path.
    // Do not add error paths, timeouts, shutdown hooks or "user closed the
    // window" handlers here. docs/SECURITY.md item S2.
    // -------------------------------------------------------------------------
    if (m_lock == nullptr) {
        return;
    }
    LOG_INFO(Lock, "authenticated; releasing the session lock");

    for (LockSurface *s : m_surfaces) {
        s->destroyProtocolObjects();
    }
    m_surfaces.clear();

    ext_session_lock_v1_unlock_and_destroy(m_lock);
    m_lock = nullptr;
    m_locked = false;
}

void SessionLock::handleLocked(void *data, ext_session_lock_v1 *lock)
{
    Q_UNUSED(lock)
    auto *self = static_cast<SessionLock *>(data);
    if (self == nullptr || self->m_locked) {
        return;
    }
    self->m_locked = true;
    LOG_INFO(Lock, "session is locked");
    Q_EMIT self->locked();
}

void SessionLock::handleFinished(void *data, ext_session_lock_v1 *lock)
{
    Q_UNUSED(lock)
    auto *self = static_cast<SessionLock *>(data);
    if (self == nullptr) {
        return;
    }
    self->m_finished = true;
    self->m_locked = false;
    LOG_ERR(Lock,
        "compositor sent ext_session_lock_v1.finished: the lock was released by someone "
        "else, or niri is shutting down. The session is no longer protected.");
    Q_EMIT self->lockLost(QStringLiteral("lock released by the compositor"));
}

// ---------------------------------------------------------------------------
// LockSurface
// ---------------------------------------------------------------------------

LockSurface::LockSurface(ext_session_lock_v1 *lock, wl_surface *surface, wl_output *output,
    SessionLock *owner)
    : m_owner(owner)
    , m_lock(lock)
    , m_surface(surface)
    , m_output(output)
{
    if (lock == nullptr || surface == nullptr) {
        return;
    }
    m_shm = QtWaylandClient::QWaylandIntegration::instance()->shm();
    if (m_shm == nullptr) {
        LOG_ERR(Lock,
            "no wl_shm: cannot allocate lock framebuffers. The compositor is missing a "
            "basic protocol.");
        return;
    }

    m_lockSurface = ext_session_lock_v1_get_lock_surface(lock, surface, output);
    if (m_lockSurface == nullptr) {
        LOG_ERR(Lock, "ext_session_lock_v1.get_lock_surface returned null");
        return;
    }
    ext_session_lock_surface_v1_add_listener(m_lockSurface, &kSurfaceListener, this);
}

LockSurface::~LockSurface()
{
    destroyProtocolObjects();
}

void LockSurface::destroyProtocolObjects()
{
    if (m_lockSurface != nullptr) {
        ext_session_lock_surface_v1_destroy(m_lockSurface);
        m_lockSurface = nullptr;
    }
    m_acked = false;
}

void LockSurface::handleConfigure(void *data, ext_session_lock_surface_v1 *lockSurface,
    uint32_t serial, uint32_t width, uint32_t height)
{
    Q_UNUSED(lockSurface)
    auto *self = static_cast<LockSurface *>(data);
    if (self == nullptr) {
        return;
    }
    const bool first = !self->m_acked;
    self->m_serial = serial;
    self->m_width = static_cast<int>(width);
    self->m_height = static_cast<int>(height);
    Q_EMIT self->configured(self->m_width, self->m_height, first);
}

void LockSurface::attachOpaqueBufferAndAcknowledge(
    const void *pixels, int bytesPerLine, int w, int h)
{
    if (m_lockSurface == nullptr || pixels == nullptr) {
        return;
    }
    const int size = bytesPerLine * h;

    // A single shared-memory buffer per frame. wl_shm requires the pool and the
    // buffer to be created client-side; the compositor only ever reads them.
    // 0600 because it holds the password-adjacent framebuffer.
    wl_shm_pool *pool = wl_shm_create_pool(m_shm, size);
    if (pool == nullptr) {
        LOG_ERR(Lock, "wl_shm_create_pool failed");
        return;
    }
    void *addr = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE,
        MAP_SHARED, wl_shm_pool_create_buffer(pool, 0, w, h, bytesPerLine,
            WL_SHM_FORMAT_ARGB8888),
        0);
    if (addr == MAP_FAILED) {
        wl_shm_pool_destroy(pool);
        LOG_ERR(Lock, "mmap of the lock framebuffer failed");
        return;
    }
    ::memcpy(addr, pixels, static_cast<size_t>(size));

    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, bytesPerLine,
        WL_SHM_FORMAT_ARGB8888);

    wl_surface_attach(m_surface, buffer, 0, 0);
    wl_surface_damage(m_surface, 0, 0, w, h);

    // One commit with the buffer attached *before* the ack. The protocol
    // requires that we not acknowledge a configure without having a buffer ready
    // for it, otherwise the compositor unmaps our surface and reveals the
    // desktop behind it.
    wl_surface_commit(m_surface);
    ext_session_lock_surface_v1_ack_configure(m_lockSurface, m_serial);
    m_acked = true;

    // We do not need to keep the pool or the buffer alive; the compositor holds
    // its own reference to the buffer after commit.
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    ::munmap(addr, static_cast<size_t>(size));
}

void LockSurface::updateOpaqueBuffer(const void *pixels, int bytesPerLine, int w, int h)
{
    if (m_lockSurface == nullptr || pixels == nullptr) {
        return;
    }
    const int size = bytesPerLine * h;

    wl_shm_pool *pool = wl_shm_create_pool(m_shm, size);
    if (pool == nullptr) {
        return;
    }
    void *addr = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE,
        MAP_SHARED, wl_shm_pool_create_buffer(pool, 0, w, h, bytesPerLine,
            WL_SHM_FORMAT_ARGB8888),
        0);
    if (addr == MAP_FAILED) {
        wl_shm_pool_destroy(pool);
        return;
    }
    ::memcpy(addr, pixels, static_cast<size_t>(size));
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, bytesPerLine,
        WL_SHM_FORMAT_ARGB8888);

    wl_surface_attach(m_surface, buffer, 0, 0);
    wl_surface_damage(m_surface, 0, 0, w, h);
    wl_surface_commit(m_surface);

    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    ::munmap(addr, static_cast<size_t>(size));
}

} // namespace kapah