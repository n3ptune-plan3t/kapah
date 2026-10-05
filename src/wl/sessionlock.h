// ext-session-lock-v1 client for kapah-lock.
//
// SECURITY (docs/SECURITY.md, checklist item S1):
//
//   The whole point of ext-session-lock-v1 is that the *compositor* owns the
//   locked state, not this process. Once niri has sent `locked`, it keeps every
//   other client frozen and keeps the outputs black until we send
//   `unlock_and_destroy`. If this process is killed, crashes, hangs or is
//   SIGKILLed, the session STAYS locked. That is the protocol working as
//   designed and it is the reason ADR-001 puts the lock screen in its own
//   binary.
//
//   Therefore there must be no code path -- none -- that unlocks the session
//   without an authenticated password. In particular:
//     * no "quit after timeout"
//     * no "unlock if the shell dies"
//     * no error handler that calls unlock_and_destroy
//   `SessionLock::unlockAndDestroy()` is called from exactly one place: the
//   authentication success path in LockView. It is private, and the .cpp says so.
//
//   The one thing that *is* safe: acknowledging a configure before the surface
//   has painted a solid colour would let the compositor unmap the surface and
//   show the frozen desktop behind it. We ack only from paintEvent(), after a
//   fully opaque frame has been attached.
//
// Sequence, from ext-session-lock-v1.xml:
//   client: ext_session_lock_manager_v1.lock() -> new_id (the lock)
//   client: lock.get_lock_surface(wl_surface, output) -> ext_session_lock_surface_v1
//   compositor: lock_surface.configure(serial, width, height)  [once per surface]
//   client:   paint opaque, then lock_surface.ack_configure(serial)
//   compositor: lock.locked
//   ... user authenticates ...
//   client: lock.unlock_and_destroy()

#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

struct ext_session_lock_manager_v1;
struct ext_session_lock_v1;
struct ext_session_lock_surface_v1;
struct wl_shm;
struct wl_surface;
struct wl_output;

namespace kapah {

class LockSurface;

// Owns the lock. One per process.
class SessionLock : public QObject {
    Q_OBJECT
public:
    explicit SessionLock(QObject *parent = nullptr);
    ~SessionLock() override;

    // Binds ext_session_lock_manager_v1 and calls lock(). No output surfaces are
    // created yet: the caller adds them with createSurface().
    bool begin();

    // True once the compositor sent `locked`. Until then the session is in a
    // half-locked transition and we must keep painting.
    bool isLocked() const { return m_locked; }
    // True once the compositor sent `finished`, which means the lock is gone
    // (another client locked, or niri is shutting down). There is no recovery.
    bool isFinished() const { return m_finished; }

    bool available() const { return m_manager != nullptr; }

    // Creates a lock surface for `output`. Returns nullptr on failure; the caller
    // reports it and the lock stays held (which is the safe direction).
    LockSurface *createSurface(wl_surface *surface, wl_output *output);
    void destroySurface(LockSurface *surface);

    int surfaceCount() const { return static_cast<int>(m_surfaces.size()); }

    // True when every created surface has been configured *and* acked. niri
    // sends `locked` only once the client has acknowledged all of them.
    bool allSurfacesAcknowledged() const;

Q_SIGNALS:
    void locked();
    void finished();
    // The compositor asked every client to stop; niri is going away or another
    // lock took over. The UI must stop claiming the session is protected.
    void lockLost(const QString &reason);
    void surfaceAdded(LockSurface *surface);

private:
    friend class LockSurface;
    friend class LockView;

    // SECURITY: reachable only from LockView's authentication success path.
    // See the header comment before changing its call sites.
    void unlockAndDestroy();

    static void handleLocked(void *data, ext_session_lock_v1 *lock);
    static void handleFinished(void *data, ext_session_lock_v1 *lock);

    ext_session_lock_manager_v1 *m_manager = nullptr;
    ext_session_lock_v1 *m_lock = nullptr;
    bool m_locked = false;
    bool m_finished = false;
    QSet<LockSurface *> m_surfaces;
};

// One ext_session_lock_surface_v1.
class LockSurface : public QObject {
    Q_OBJECT
public:
    ~LockSurface() override;

    wl_surface *surface() const { return m_surface; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    quint32 serial() const { return m_serial; }
    bool acknowledged() const { return m_acked; }

    // Attaches an opaque buffer and commits it, then acknowledges. Call this
    // *after* painting an opaque frame -- see the header comment.
    void attachOpaqueBufferAndAcknowledge(const void *pixels, int bytesPerLine, int w, int h);
    // Damages and commits a new opaque buffer without re-acking.
    void updateOpaqueBuffer(const void *pixels, int bytesPerLine, int w, int h);

Q_SIGNALS:
    // A new configure arrived. `first` distinguishes the mandatory initial one.
    void configured(int width, int height, bool first);

private:
    friend class SessionLock;
    LockSurface(ext_session_lock_v1 *lock, wl_surface *surface, wl_output *output, SessionLock *owner);

    static void handleConfigure(void *data, ext_session_lock_surface_v1 *lockSurface,
        uint32_t serial, uint32_t width, uint32_t height);

    SessionLock *m_owner = nullptr;
    wl_shm *m_shm = nullptr;
    ext_session_lock_v1 *m_lock = nullptr;
    ext_session_lock_surface_v1 *m_lockSurface = nullptr;
    wl_surface *m_surface = nullptr;
    wl_output *m_output = nullptr;

    quint32 m_serial = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_acked = false;
};

} // namespace kapah