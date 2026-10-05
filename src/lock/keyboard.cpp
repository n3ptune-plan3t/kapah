#include "keyboard.h"

#include "logging.h"

#include <wayland-client.h>

#include <cstring>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

namespace kapah {

namespace {

const wl_seat_listener kSeatListener = {
    [](void *, const char *) {},
    [](void *data, wl_seat *seat, uint32_t caps) {
        auto *self = static_cast<LockKeyboard *>(data);
        self->onSeatCaps(caps, seat);
    },
};

const wl_keyboard_listener kKeyboardListener = {
    [](void *data, wl_keyboard *, quint32, wl_surface *surface, void *) {
        static_cast<LockKeyboard *>(data)->onSurfaceActivated(surface);
    },
    [](void *data, wl_keyboard *, quint32, wl_surface *surface) {
        static_cast<LockKeyboard *>(data)->onSurfaceLeft(surface);
    },
    [](void *data, wl_keyboard *, quint32 format, int fd, quint32 size) {
        static_cast<LockKeyboard *>(data)->onKeymap(format, fd, size);
    },
    nullptr, // repeat_info is unused at launch
    [](void *data, wl_keyboard *, quint32, quint32, quint32 keycode, quint32 state) {
        static_cast<LockKeyboard *>(data)->onKey(keycode, state);
    },
    [](void *data, wl_keyboard *, quint32, quint32 depressed, quint32 latched, quint32 locked,
        quint32 group) {
        static_cast<LockKeyboard *>(data)->onModifiers(depressed, latched, locked, group);
    },
};

} // namespace

LockKeyboard::LockKeyboard(wl_seat *seat, QObject *parent)
    : QObject(parent)
    , m_seat(seat)
{
    m_ctx = xkb_context_new(XKB_CONTEXT_COMPILE_NO_FLAGS);

    if (m_seat != nullptr) {
        wl_seat_add_listener(m_seat, const_cast<wl_seat_listener *>(&kSeatListener), this);
        // One roundtrip at startup so the capabilities (and therefore the
        // wl_keyboard) are known before the first frame.
        wl_display_roundtrip(wl_seat_get_display(m_seat));
    }

    if (m_keyboard == nullptr) {
        LOG_WARN(Lock, "no wl_seat with keyboard: password entry unavailable");
    }
}

LockKeyboard::~LockKeyboard()
{
    if (m_state != nullptr) {
        xkb_state_unref(m_state);
    }
    if (m_keymap != nullptr) {
        xkb_keymap_unref(m_keymap);
    }
    if (m_ctx != nullptr) {
        xkb_context_unref(m_ctx);
    }
    if (m_keyboard != nullptr) {
        wl_keyboard_destroy(m_keyboard);
    }
    if (m_seat != nullptr) {
        wl_seat_destroy(m_seat);
    }
}

void LockKeyboard::onSeatCaps(uint32_t caps, wl_seat *seat)
{
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && m_keyboard == nullptr) {
        m_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(m_keyboard, const_cast<wl_keyboard_listener *>(&kKeyboardListener),
            this);
    }
}

void LockKeyboard::onKeymap(quint32 format, int fd, quint32 size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        LOG_WARN(Lock, "unsupported keymap format: the input map may be wrong");
        return;
    }
    void *p = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        return;
    }
    if (m_keymap != nullptr) {
        xkb_keymap_unref(m_keymap);
        m_keymap = nullptr;
    }
    if (m_state != nullptr) {
        xkb_state_unref(m_state);
        m_state = nullptr;
    }
    m_keymap = xkb_keymap_new_from_string(m_ctx, static_cast<const char *>(p),
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(p, static_cast<std::size_t>(size));
    close(fd);
    if (m_keymap != nullptr) {
        m_state = xkb_state_new(m_keymap);
    }
}

void LockKeyboard::onKey(quint32 keycode, quint32 state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        return;
    }
    if (m_state == nullptr) {
        return;
    }
    const quint32 kc = keycode + 8; // XKB keycodes are wayland keycodes + 8.
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_state, kc);
    if (sym == XKB_KEY_BackSpace) {
        Q_EMIT backspace();
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        Q_EMIT enter();
        return;
    }
    const uint32_t cp = xkb_state_key_get_utf32(m_state, kc);
    if (cp != 0) {
        Q_EMIT character(QString(QChar::fromUcs4(cp)));
    }
}

void LockKeyboard::onModifiers(quint32 depressed, quint32 latched, quint32 locked, quint32 group)
{
    if (m_state == nullptr) {
        return;
    }
    xkb_state_update_mask(m_state, depressed, latched, locked, 0, 0, group);
    const xkb_mod_index_t caps = xkb_keymap_mod_get_index(m_keymap, XKB_MOD_NAME_CAPS);
    const bool capsOn = caps != XKB_MOD_INVALID
        && xkb_state_mod_index_is_active(m_state, caps, XKB_STATE_MODS_LOCKED);
    const xkb_mod_index_t shift = xkb_keymap_mod_get_index(m_keymap, XKB_MOD_NAME_SHIFT);
    const bool shiftOn = shift != XKB_MOD_INVALID
        && xkb_state_mod_index_is_active(m_state, shift,
            static_cast<xkb_state_component>(XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED
                | XKB_STATE_MODS_LOCKED));
    Q_EMIT modifiersChanged(capsOn, shiftOn);
}

void LockKeyboard::onSurfaceActivated(wl_surface *surface)
{
    Q_EMIT surfaceActivated(surface);
}

void LockKeyboard::onSurfaceLeft(wl_surface *surface)
{
    Q_EMIT surfaceDeactivated(surface);
}

} // namespace kapah
