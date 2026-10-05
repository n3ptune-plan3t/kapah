// zwlr_layer_shell_v1 client: in-tree bindings plus the QWaylandWindow subclass
// that turns a QWidget into a layer surface.
//
// ADR-002 explains why this is generated in-tree rather than pulled from
// LayerShellQt. The short version: the API is 12 requests, we need per-output
// configuration that a global-singleton wrapper makes awkward, and a vendored
// XML keeps the distro dependency list at three.
//
// Contract:
//   * Configuration must be reachable from WaylandWindow::initWindow(), because
//     wlr-layer-shell forbids changing anchor/size/margins/exclusive-zone/
//     keyboard-interactivity/layer after the first configure. See
//     LayerShellRegistry below for how the config gets there.
//   * Nothing repaints at idle: the surface only exists while show() is called,
//     and update() is the only repaint trigger.
//
// Qt symbols this file depends on (private, verified against Qt 6.5.3 and
// 6.8.x; see docs/PROTOCOLS.md for the probe output):
//   QtWaylandClient::QWaylandWindow        (+ initWindow/initWindowCopy/setVisible/
//                                          setInterface/flags/registerPlugin)
//   QtWaylandClient::QWaylandWindowPlugin  (+ create/supportsWindow)
//   QtWaylandClient::QWaylandShellSurface  (base of LayerSurface)
//   QtWaylandClient::QWaylandSurfaceConfigure
//   QtWaylandClient::QWaylandIntegration   (+ display/compositor/layerShell/
//                                          hasCapability/Capability::LayerShell)
//   QtWaylandClient::QWaylandDisplay       (+ display())
//   QtWaylandClient::QWaylandScreen        (+ output())

#pragma once

#include "qtcompat.h"

#include <QMargins>
#include <QWidget>

#include <memory>

class QScreen;
class QWindow;

namespace kapah {

class LayerSurface;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class Layer : int {
    Background = 0,
    Bottom = 1,
    Top = 2,
    Overlay = 3,
};

enum class KeyboardInteractivity : int {
    None = 0,
    Exclusive = 1,
    OnDemand = 2,
};

enum class AnchorFlag : int {
    None = 0,
    Top = 1,
    Bottom = 2,
    Left = 4,
    Right = 8,
};
Q_DECLARE_FLAGS(Anchors, AnchorFlag)
Q_FLAG(Anchors)

// Everything zwlr_layer_surface_v1 needs, in one copyable struct so it can move
// into the registry without refcount traffic.
struct LayerConfig {
    QString namespaceString = QStringLiteral("kapah");

    Layer layer = Layer::Top;
    Anchors anchors = AnchorFlag::Top | AnchorFlag::Left | AnchorFlag::Right;

    // Logical pixels. For a surface anchored to both horizontal edges and one
    // vertical edge, height is the meaningful one and width is ignored.
    int width = 0;
    int height = 0;

    // -1 = "ignore other exclusive zones" (wlr semantics); 0 = "float above
    // other exclusive zones"; >0 = reserve this many logical pixels.
    int exclusiveZone = 0;

    QMargins margins;

    KeyboardInteractivity keyboard = KeyboardInteractivity::None;

    // Empty = bind to whichever output the QWindow's screen resolves to. Set
    // explicitly when the surface must not follow its screen.
    QString outputName;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// Resolves "which LayerConfig belongs to this QWindow".
//
// The platform window -- and therefore the layer surface -- is created inside
// QWindow::create(), which happens before a widget gets a chance to configure
// itself. Two mechanisms cover the two possible orderings of QWidget's lazy
// handle creation across Qt 6.5-6.8:
//
//   1. LayerShellWindow binds its config to the QWindow it obtains in its
//      constructor. If that QWindow has not been created yet (the usual case),
//      initWindow() looks it up here.
//   2. If the platform window was already created by the time we bind -- which
//      happens when QWidget::windowHandle() eagerly creates it -- initWindow()
//      falls back to consuming the config from a FIFO queue. Qt creates platform
//      windows in the same order the widgets push, so the FIFO is still correct.
//
// Mechanism 2 is a safety net, not the main path; the fallback logs a warning
// exactly once per process so a regression here is visible.
class LayerShellRegistry {
public:
    static void bind(QWindow *window, const LayerConfig &config);
    static void unbind(QWindow *window);

    // Pushes a config for a window whose QWindow is not known yet.
    static void enqueuePending(const LayerConfig &config);

    // Called from WaylandWindow::initWindow(). Returns nullptr when the window
    // was never registered, which the caller treats as "use layer defaults".
    static const LayerConfig *lookup(QWindow *window);

    // Consumes the oldest pending config, if any.
    static const LayerConfig *consumePending();

    // Called once when the FIFO fallback is used, so a regression in the primary
    // mechanism is visible in the log without spamming it.
    static void noteFallback();

    // Test hook: drops all state.
    static void reset();
};

// ---------------------------------------------------------------------------
// LayerSurface -- one zwlr_layer_surface_v1
// ---------------------------------------------------------------------------

class LayerSurface : public QtWaylandClient::QWaylandShellSurface {
    Q_OBJECT
public:
    LayerSurface(QWindow *window, const LayerConfig &config);
    ~LayerSurface() override;

    const LayerConfig &config() const { return m_config; }

    // Re-applies the state requests. Only legal before the first configure is
    // acknowledged; LayerSurface enforces this and logs if it is violated.
    void applyConfig();

    // Resizes and commits. Safe at any time (set_size is legal after configure;
    // the compositor answers with a new configure if the size is unacceptable).
    void requestSize(int width, int height);

    // Damage + commit. The only wakeup source for this surface.
    void commitDamage();

    // Commits with no buffer so the compositor maps the surface.
    void showSurface();
    // Destroys the layer surface and its wl_surface. Idempotent.
    void hideSurface();

    bool isConfigured() const { return m_configured; }

Q_SIGNALS:
    // First configure arrived and was acknowledged. Used for startup timing.
    void firstConfigure(int width, int height);
    // The compositor refused to keep the surface (output gone, protocol error).
    void closedByCompositor();
    // The compositor asked for a different size.
    void resized(int width, int height);

protected:
    void handleConfigure(const QtWaylandClient::QWaylandSurfaceConfigure &configure) override;

private:
    void sendStateRequests();

    QWindow *m_window = nullptr;
    LayerConfig m_config;

    wl_surface *m_surface = nullptr;
    zwlr_layer_surface_v1 *m_layerSurface = nullptr;

    bool m_configured = false;
};

// ---------------------------------------------------------------------------
// LayerShellWindow -- a QWidget that lives on a layer surface
// ---------------------------------------------------------------------------

// Usage:
//     auto *bar = new LayerShellWindow(cfg);
//     new MyBarWidget(bar);        // reparented, fills the surface
//     bar->showForScreen(screen);
//
// The widget is frameless and undecorated: it is a QWindow underneath, so Qt's
// window decorations do not apply (ADR-002 also sets
// QT_WAYLAND_DISABLE_WINDOWDECORATION=1 as a backstop).
class LayerShellWindow : public QWidget {
    Q_OBJECT
public:
    explicit LayerShellWindow(const LayerConfig &config, QWidget *parent = nullptr);
    ~LayerShellWindow() override;

    // Replaces the configuration. Because the protocol forbids changing anchor,
    // layer or exclusive-zone after configure, applying this to a live surface
    // recreates it. Call it only when something actually changed.
    void setLayerConfig(const LayerConfig &config);
    const LayerConfig &layerConfig() const { return m_config; }

    // Convenience setters; each goes through setLayerConfig().
    void setLayer(Layer layer);
    void setAnchors(Anchors anchors);
    void setExclusiveZone(int zone);
    void setMargins(const QMargins &margins);
    void setKeyboardInteractivity(KeyboardInteractivity interactivity);
    void setNamespaceString(const QString &ns);
    void setOutputName(const QString &outputName);

    // Sizes the surface and places it on a specific output. Passing nullptr
    // uses the widget's current screen.
    void showForScreen(QScreen *screen);

    // Destroys the platform surface but keeps the widget alive, so a later
    // show() rebuilds it. This is how the notification stack achieves "zero
    // surface cost at idle" (Phase 4.8).
    void releaseSurface();
    bool hasSurface() const;

    // Forces a repaint of the surface. No-op when the surface does not exist.
    void requestPaint();

Q_SIGNALS:
    // The compositor closed the surface (output unplugged, protocol error).
    void surfaceClosed();
    // Emitted once the first configure has been acknowledged.
    void surfaceConfigured();
    // The compositor asked for a different size.
    void surfaceResized(int width, int height);

protected:
    bool event(QEvent *event) override;

private:
    void ensureSurface();

    LayerConfig m_config;
    bool m_creating = false;
};

// Installs the QWaylandWindow plugin that makes QWindow create our subclass.
// Idempotent. Must be called after the QGuiApplication exists and before the
// first layer surface is created.
void installLayerShellPlugin();

// True when the compositor advertises zwlr_layer_shell_v1. Capability detection,
// not an assumption (roadmap section 6).
bool layerShellAvailable();

// Capability snapshot for docs/PROTOCOLS.md and for `kapahctl status`.
struct WaylandCapabilities {
    bool layerShell = false;
    quint32 layerShellVersion = 0;
    bool sessionLock = false;
    bool idleNotify = false;
    bool dataControl = false;
    bool gammaControl = false;
    bool outputPower = false;
};
WaylandCapabilities queryCapabilities();

} // namespace kapah