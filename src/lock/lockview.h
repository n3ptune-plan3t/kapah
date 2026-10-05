// The lock surface contents and the authentication state machine.
//
// SECURITY (docs/SECURITY.md, items S1-S4): this is the only caller of
// SessionLock::unlockAndDestroy(). The sequence is exactly
//   PAM success -> unlockAndDestroy() -> exit(0)
// and there is no timeout branch, no "close" path, and no error branch that
// unlocks. If PAM fails, the only thing that happens is the field clears and
// an error is shown.
//
// Painting discipline: the very first frame after each configure is fully
// opaque (ARGB8888, alpha 255 everywhere) and is what we ack the configure
// with; until that ack the compositor cannot map the surface, so no frame of
// the underlying session ever shows through. Subsequent updates use
// updateOpaqueBuffer() (no re-ack).
//
// The clock follows the same single-shot, minute-aligned, VeryCoarseTimer
// discipline as the bar clock; surfaces repaint only on configure, on a clock
// tick, or on an IPC-driven layout change.

#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include "passwordfield.h"

struct wl_surface;

class QScreen;

namespace kapah {

class LockKeyboard;
class PamAuthenticator;
class SessionLock;
class Theme;

namespace niri {
class NiriState;
}

class LockView : public QObject {
    Q_OBJECT
public:
    explicit LockView(SessionLock *lock, niri::NiriState *state, Theme *theme,
        PamAuthenticator *authenticator, QObject *parent = nullptr);
    ~LockView() override;

    void setKeyboard(LockKeyboard *kb);
    void addScreen(QScreen *screen);
    void removeScreen(QScreen *screen);

private:
    struct PerScreen {
        QScreen *screen = nullptr;
        wl_surface *surface = nullptr;
        class LockSurface *surfaceV1 = nullptr;
        QImage *frame = nullptr;
        int width = 0;
        int height = 0;
        bool firstConfigureSeen = false;
    };

    PerScreen *screenForSurface(wl_surface *surface);
    void render(PerScreen *ps);
    void onConfigured(PerScreen *ps, int width, int height, bool first);
    void onClockTick();
    void armClock();
    void onKeyboardLayoutChanged();
    void startAuth();
    void onPamResult(bool ok, int pamError);

    SessionLock *m_lock = nullptr;
    niri::NiriState *m_state = nullptr;
    Theme *m_theme = nullptr;
    PamAuthenticator *m_pam = nullptr;
    LockKeyboard *m_kb = nullptr;
    PasswordField m_field;
    QTimer m_clockTimer;
    bool m_caps = false;
    QString m_error;
    QList<PerScreen *> m_screens;
    PerScreen *m_inputSurface = nullptr; // the one the keyboard activated
};

} // namespace kapah
