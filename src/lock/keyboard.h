// Raw keyboard input for kapah-lock: wl_keyboard + xkbcommon.
//
// Why not QWidget: the lock surface is an ext_session_lock_surface_v1 backed
// by a raw wl_surface, not a QWindow, so Qt never delivers key events to it.
// The standard path is wl_keyboard -> xkbcommon, which is also what every
// non-Qt Wayland lock client does. Caps-lock state comes from the XKB
// modifiers, so there is no evdev and no polling anywhere.

#pragma once

#include <QObject>

struct wl_display;
struct wl_seat;
struct wl_keyboard;
struct wl_surface;

namespace kapah {

class LockKeyboard : public QObject {
    Q_OBJECT
public:
    // Pass the process-wide wl_seat (Globals::seat()). Caps-lock and input
    // come from this seat's wl_keyboard; the seat is bound once per process.
    explicit LockKeyboard(wl_seat *seat, QObject *parent = nullptr);
    ~LockKeyboard() override;

    // True once a wl_seat with a wl_keyboard appeared. If false, the lock
    // screen cannot accept input at all; LockView shows an unavailable note
    // rather than hanging.
    bool available() const { return m_keyboard != nullptr; }
    wl_seat *seat() const { return m_seat; }
    void setSeat(wl_seat *seat) { m_seat = seat; }

    // Entry points the plain-C wayland listeners forward to. Public because
    // the listener structs live in the anonymous namespace of the .cpp.
    void onKeymap(quint32 format, int fd, quint32 size);
    void onKey(quint32 keycode, quint32 state);
    void onModifiers(quint32 depressed, quint32 latched, quint32 locked, quint32 group);
    void onSurfaceActivated(wl_surface *surface);
    void onSurfaceLeft(wl_surface *surface);
    void onSeatCaps(uint32_t caps, wl_seat *seat);

Q_SIGNALS:
    void surfaceActivated(wl_surface *surface);
    void surfaceDeactivated(wl_surface *surface);
    void character(const QString &text);
    void backspace();
    void enter();
    void modifiersChanged(bool capsLock, bool shift);

private:
    wl_display *m_display = nullptr;
    wl_seat *m_seat = nullptr;
    wl_keyboard *m_keyboard = nullptr;
    struct xkb_context *m_ctx = nullptr;
    struct xkb_keymap *m_keymap = nullptr;
    struct xkb_state *m_state = nullptr;
};

} // namespace kapah
