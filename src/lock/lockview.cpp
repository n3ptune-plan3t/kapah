#include "lockview.h"

#include "keyboard.h"
#include "pam.h"
#include "sessionlock.h"
#include "state.h"
#include "theme.h"

#include "logging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QPainter>
#include <QTime>

#include "qtcompat.h"

#include <QtWaylandClient/private/qwaylandintegration_p.h>

#include <wayland-client.h>

#include <cstring>

namespace kapah {

namespace niri {
class NiriState;
}

LockView::LockView(SessionLock *lock, niri::NiriState *state, Theme *theme,
    PamAuthenticator *authenticator, QObject *parent)
    : QObject(parent)
    , m_lock(lock)
    , m_state(state)
    , m_theme(theme)
    , m_pam(authenticator)
{
    m_clockTimer.setSingleShot(true);
    m_clockTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_clockTimer, &QTimer::timeout, this, &LockView::onClockTick);

    if (m_state != nullptr) {
        connect(m_state, &niri::NiriState::keyboardLayoutsChanged, this,
            &LockView::onKeyboardLayoutChanged);
        connect(m_state, &niri::NiriState::keyboardLayoutSwitched, this,
            &LockView::onKeyboardLayoutChanged);
    }
    if (m_pam != nullptr) {
        connect(m_pam, &PamAuthenticator::result, this, &LockView::onPamResult);
    }
}

LockView::~LockView()
{
    for (PerScreen *ps : m_screens) {
        delete ps->frame;
        delete ps;
    }
}

void LockView::setKeyboard(LockKeyboard *kb)
{
    m_kb = kb;
    if (m_kb == nullptr) {
        return;
    }
    connect(m_kb, &LockKeyboard::character, this, [this](const QString &text) {
        m_error.clear();
        for (const QChar &c : text) {
            m_field.insert(c);
        }
        // Re-render the surface that owns the keyboard focus, or all of them;
        // the field is one-for-all, so repaint every configured surface.
        for (PerScreen *ps : m_screens) {
            if (ps->firstConfigureSeen) {
                render(ps);
                ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                    ps->width, ps->height);
            }
        }
    });
    connect(m_kb, &LockKeyboard::backspace, this, [this] {
        m_error.clear();
        m_field.backspace();
        for (PerScreen *ps : m_screens) {
            if (ps->firstConfigureSeen) {
                render(ps);
                ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                    ps->width, ps->height);
            }
        }
    });
    connect(m_kb, &LockKeyboard::enter, this, &LockView::startAuth);
    connect(m_kb, &LockKeyboard::modifiersChanged, this,
        [this](bool capsLock, bool) {
            m_caps = capsLock;
            for (PerScreen *ps : m_screens) {
                if (ps->firstConfigureSeen) {
                    render(ps);
                    ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                        ps->width, ps->height);
                }
            }
        });
    connect(m_kb, &LockKeyboard::surfaceActivated, this, [this](wl_surface *surface) {
        m_inputSurface = screenForSurface(surface);
    });
    connect(m_kb, &LockKeyboard::surfaceDeactivated, this, [this](wl_surface *surface) {
        if (m_inputSurface != nullptr && m_inputSurface->surface == surface) {
            m_inputSurface = nullptr;
        }
    });
}

LockView::PerScreen *LockView::screenForSurface(wl_surface *surface)
{
    for (PerScreen *ps : m_screens) {
        if (ps->surface == surface) {
            return ps;
        }
    }
    return nullptr;
}

void LockView::addScreen(QScreen *screen)
{
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    wl_compositor *compositor = integration != nullptr ? integration->compositor() : nullptr;
    wl_output *output = wlcompat::outputForScreen(screen);
    if (compositor == nullptr || output == nullptr) {
        LOG_ERR(Lock, "cannot create a lock surface: no compositor or wl_output");
        return;
    }
    auto *ps = new PerScreen;
    ps->screen = screen;
    ps->surface = wl_compositor_create_surface(compositor);
    ps->surfaceV1 = m_lock->createSurface(ps->surface, output);
    if (ps->surfaceV1 == nullptr) {
        LOG_ERR(Lock, "SessionLock::createSurface failed; the session stays locked (safe)");
        wl_surface_destroy(ps->surface);
        delete ps;
        return;
    }
    connect(ps->surfaceV1, &LockSurface::configured, this,
        [this, ps](int width, int height, bool first) { onConfigured(ps, width, height, first); });
    m_screens.append(ps);
}

void LockView::removeScreen(QScreen *screen)
{
    for (int i = 0; i < m_screens.size(); ++i) {
        if (m_screens.at(i)->screen == screen) {
            PerScreen *ps = m_screens.takeAt(i);
            delete ps->frame;
            delete ps;
            break;
        }
    }
}

void LockView::onConfigured(PerScreen *ps, int width, int height, bool /*first*/)
{
    ps->width = width;
    ps->height = height;
    if (ps->frame == nullptr) {
        ps->frame = new QImage;
    }
    *ps->frame = QImage(width, height, QImage::Format_ARGB32);
    ps->frame->fill(Qt::black);
    if (!ps->firstConfigureSeen) {
        ps->firstConfigureSeen = true;
        render(ps);
        ps->surfaceV1->attachOpaqueBufferAndAcknowledge(ps->frame->constBits(),
            ps->frame->bytesPerLine(), width, height);
    } else {
        render(ps);
        ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
            width, height);
    }
    armClock();
}

void LockView::render(PerScreen *ps)
{
    if (ps->frame == nullptr || ps->width <= 0 || ps->height <= 0) {
        return;
    }
    QPainter p(ps->frame);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Fully opaque frame: nothing from the underlying session can ever
    // composite through, which is the point of the solid fill even at idle.
    p.fillRect(ps->frame->rect(), m_theme->color(palette::Background));

    const QRect full(0, 0, ps->width, ps->height);
    p.setPen(m_theme->color(palette::Foreground));
    auto f = m_theme->font(3);
    p.setFont(f);
    const QDateTime now = QDateTime::currentDateTime();
    p.drawText(full.adjusted(0, -full.height() / 4, 0, -full.height() / 4), Qt::AlignCenter,
        now.toString(QStringLiteral("HH:mm")));
    auto fSmall = m_theme->font(1);
    p.setFont(fSmall);
    p.setPen(m_theme->color(palette::Muted));
    p.drawText(full.adjusted(0, -full.height() / 8, 0, -full.height() / 8), Qt::AlignCenter,
        now.toString(QStringLiteral("dddd, MMMM d")));

    // Password pill, centered, below middle.
    const int pw = m_theme->px(280);
    const int ph = m_theme->px(44);
    const QRect pill(ps->width / 2 - pw / 2, ps->height / 2 + m_theme->px(40), pw, ph);
    m_field.paint(p, pill, *m_theme);

    // Caps-lock indicator, just below the pill.
    if (m_caps) {
        p.setPen(m_theme->color(palette::Warning));
        p.setFont(fSmall);
        p.drawText(QRect(ps->width / 2 - pw / 2, pill.bottom() + m_theme->px(8), pw,
                   m_theme->px(20)),
            Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Caps Lock is on"));
    }

    // Keyboard layout, bottom-left, from niri IPC (never evdev, never a timer).
    if (m_state != nullptr) {
        const QString layout = m_state->keyboardLayouts().currentDisplayName();
        if (!layout.isEmpty()) {
            p.setPen(m_theme->color(palette::Muted));
            p.setFont(fSmall);
            p.drawText(QRect(m_theme->px(16), ps->height - m_theme->px(32), ps->width / 2,
                       m_theme->px(20)),
                Qt::AlignLeft | Qt::AlignVCenter, layout);
        }
    }

    // Error message.
    if (!m_error.isEmpty()) {
        p.setPen(m_theme->color(palette::Urgent));
        p.setFont(fSmall);
        p.drawText(QRect(ps->width / 2 - pw / 2, pill.top() - m_theme->px(28), pw,
                   m_theme->px(20)),
            Qt::AlignHCenter | Qt::AlignTop, m_error);
    }

    if (m_kb == nullptr || !m_kb->available()) {
        p.setPen(m_theme->color(palette::Urgent));
        p.setFont(fSmall);
        p.drawText(QRect(0, ps->height - m_theme->px(32), ps->width, m_theme->px(20)),
            Qt::AlignCenter, QStringLiteral("No input device detected"));
    }
}

void LockView::onClockTick()
{
    for (PerScreen *ps : m_screens) {
        if (ps->firstConfigureSeen) {
            render(ps);
            ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                ps->width, ps->height);
        }
    }
    armClock();
}

void LockView::armClock()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime nextMinute = QDateTime(now.date(),
        QTime(now.time().hour(), now.time().minute(), 0)).addSecs(60);
    const qint64 ms = now.msecsTo(nextMinute);
    m_clockTimer.start(qMax(ms, (qint64)1));
}

void LockView::onKeyboardLayoutChanged()
{
    for (PerScreen *ps : m_screens) {
        if (ps->firstConfigureSeen) {
            render(ps);
            ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                ps->width, ps->height);
        }
    }
}

void LockView::startAuth()
{
    if (m_pam == nullptr || m_field.isEmpty() || m_pam->attemptInFlight()) {
        return;
    }
    QString password = m_field.takePassword();
    m_pam->start(password);
    // Scrub the local copy; takePassword() already scrubbed the field's.
    // The copy held by the queued invokeMethod event cannot be scrubbed and is
    // freed once run() is invoked on the worker.
    // (Lifecycle note in docs/SECURITY.md, S4.)
    explicit_bzero(password.data(), static_cast<std::size_t>(password.size()) * sizeof(char16_t));
    password.clear();
}

void LockView::onPamResult(bool ok, int pamError)
{
    if (ok) {
        // The only unlock path in the process:
        //   pam.result(true) -> unlockAndDestroy() -> exit.
        m_lock->unlockAndDestroy();
        if (auto *integration = QtWaylandClient::QWaylandIntegration::instance()) {
            if (auto *wd = integration->display()) {
                wl_display_flush(wd->display());
            }
        }
        QCoreApplication::exit(0);
    } else {
        LOG_WARNF(Lock, "PAM rejected password, code %d", pamError);
        m_error = QStringLiteral("Authentication failed");
        m_field.clear();
        for (PerScreen *ps : m_screens) {
            if (ps->firstConfigureSeen) {
                render(ps);
                ps->surfaceV1->updateOpaqueBuffer(ps->frame->constBits(), ps->frame->bytesPerLine(),
                    ps->width, ps->height);
            }
        }
    }
}

} // namespace kapah
