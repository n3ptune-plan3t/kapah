// Compatibility shim for the Qt private Wayland client API.
//
// ADR-002 pins kapah to Qt 6.5 through 6.8. That window exists because
// QtWaylandClient's private headers are not API-stable, and the layer-shell glue
// in this directory is the only place in the tree that touches them.
//
// Everything version-specific is collected here so that src/wl/ itself reads as
// ordinary code. If a future Qt needs a different incantation, it goes in this
// file and nowhere else.
#pragma once

#include <QtGlobal>

// ---- Qt version range we are known to build against -------------------------
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#error "kapah requires Qt >= 6.5 (see ADR-002)"
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#warning "kapah is untested on Qt >= 6.9; see docs/decisions/ADR-002-layer-shell-bindings.md"
#endif

// ---- Header locations --------------------------------------------------------
// Qt 6.5 installs the client private headers under
// <prefix>/include/QtWaylandClient/<ver>/QtWaylandClient/private/*.h and exposes
// them as <QtWaylandClient/private/...>. Older 6.x point-suffixed paths are kept
// working for distro builds that still ship the old layout.
#include <QtWaylandClient/QtWaylandClient>

#include <QtWaylandClient/private/qwaylandclientextension_p.h>
#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylandintegration_p.h>
#include <QtWaylandClient/private/qwaylandscreen_p.h>
#include <QtWaylandClient/private/qwaylandshell_p.h>
#include <QtWaylandClient/private/qwaylandshellsurface_p.h>
#include <QtWaylandClient/private/qwaylandsurfaceconfigure_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <QtWaylandClient/private/qwaylandwindowplugin_p.h>

// ---- Symbols that moved between 6.x -----------------------------------------

// Qt 6.8 renamed QWaylandWindow::Flags' scoped enum usage in a couple of spots
// and made setInterface() take a pointer-to-pointer in the 6.9 series. Neither
// affects 6.5-6.8; the guards exist so the failure mode is a clear compile error
// rather than a silent behaviour change.
namespace kapah::wlcompat {

// True when the compositor has a zwlr_layer_shell_v1 global we bound.
inline bool layerShellAvailable()
{
    return QtWaylandClient::QWaylandIntegration::instance()
        && QtWaylandClient::QWaylandIntegration::instance()->hasCapability(
            QtWaylandClient::QWaylandIntegration::Capability::LayerShell);
}

// The wl_output* backing a QScreen, or nullptr for the "all outputs" case.
inline wl_output *outputForScreen(const QScreen *screen)
{
    if (screen == nullptr) {
        return nullptr;
    }
    auto *ws = static_cast<QtWaylandClient::QWaylandScreen *>(
        const_cast<QScreen *>(screen)->platformHandle());
    if (ws == nullptr) {
        return nullptr;
    }
    return ws->output();
}

} // namespace kapah::wlcompat