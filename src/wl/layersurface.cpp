#include "layersurface.h"

#include "globals.h"
#include "logging.h"

#include <QGuiApplication>
#include <QHash>
#include <QScreen>
#include <QVector>
#include <QWindow>

#include <cstdint>
#include <vector>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace kapah {

namespace {

struct RegistryState {
    QHash<const QWindow *, LayerConfig> byWindow;
    std::vector<LayerConfig> pending;
    bool fallbackLogged = false;
};

RegistryState &registry()
{
    // Function-local static: constructed on first use, which is after
    // QCoreApplication exists but before any surface is created.
    static RegistryState state;
    return state;
}

} // namespace

// ---------------------------------------------------------------------------
// LayerShellRegistry
// ---------------------------------------------------------------------------

void LayerShellRegistry::bind(QWindow *window, const LayerConfig &config)
{
    if (window == nullptr) {
        return;
    }
    registry().byWindow.insert(window, config);

    // The pending copy is only needed until we know the QWindow. Drop it so a
    // later surface cannot consume the wrong config.
    auto &pending = registry().pending;
    if (!pending.empty()) {
        const LayerConfig &back = pending.back();
        if (back.namespaceString == config.namespaceString && back.width == config.width
            && back.height == config.height && back.layer == config.layer
            && back.anchors == config.anchors && back.exclusiveZone == config.exclusiveZone
            && back.outputName == config.outputName) {
            pending.pop_back();
        }
    }
}

void LayerShellRegistry::unbind(QWindow *window)
{
    if (window != nullptr) {
        registry().byWindow.remove(window);
    }
}

void LayerShellRegistry::enqueuePending(const LayerConfig &config)
{
    registry().pending.push_back(config);
}

const LayerConfig *LayerShellRegistry::lookup(QWindow *window)
{
    if (window == nullptr) {
        return nullptr;
    }
    const auto it = registry().byWindow.constFind(window);
    return it == registry().byWindow.constEnd() ? nullptr : &it.value();
}

const LayerConfig *LayerShellRegistry::consumePending()
{
    auto &pending = registry().pending;
    if (pending.empty()) {
        return nullptr;
    }
    // Hand out a stable address: the caller uses it immediately, but a
    // thread_local slot keeps the pointer valid for the whole call.
    static LayerConfig slot;
    slot = pending.front();
    pending.erase(pending.begin());
    return &slot;
}

void LayerShellRegistry::noteFallback()
{
    if (!registry().fallbackLogged) {
        registry().fallbackLogged = true;
        LOG_WARNF(Wayland,
            "layer-shell registry: a surface used the pending-queue fallback. This "
            "should not happen on Qt 6.5-6.8; if it does, check that "
            "LayerShellWindow still binds its config before QWindow::create().");
    }
}

void LayerShellRegistry::reset()
{
    registry().byWindow.clear();
    registry().pending.clear();
    registry().fallbackLogged = false;
}

// ---------------------------------------------------------------------------
// LayerSurface
// ---------------------------------------------------------------------------

LayerSurface::LayerSurface(QWindow *window, const LayerConfig &config)
    : QtWaylandClient::QWaylandShellSurface(
        QtWaylandClient::QWaylandIntegration::instance()->shell(), window)
    , m_window(window)
    , m_config(config)
{
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr) {
        LOG_ERR(Wayland, "layer surface requested before the wayland display existed");
        return;
    }

    wl_display *display = integration->display()->display();
    wl_compositor *compositor = integration->compositor();
    if (display == nullptr || compositor == nullptr) {
        LOG_ERR(Wayland, "no wl_display/wl_compositor; cannot create a layer surface");
        return;
    }

    m_surface = wl_compositor_create_surface(compositor, display);
    if (m_surface == nullptr) {
        LOG_ERR(Wayland, "wl_compositor_create_surface returned null");
        return;
    }

    // Bind to the requested output, or to the window's screen. Passing nullptr
    // asks the compositor to choose, which for a bar is wrong on a multi-monitor
    // setup, so we always pass a real output when we can resolve one.
    wl_output *output = nullptr;
    if (!config.outputName.isEmpty()) {
        const auto screens = QGuiApplication::screens();
        for (QScreen *screen : screens) {
            if (screen->name() == config.outputName) {
                output = kapah::wlcompat::outputForScreen(screen);
                break;
            }
        }
        if (output == nullptr) {
            LOG_WARNF(Wayland, "layer surface for unknown output '%s'",
                qPrintable(config.outputName));
        }
    }
    if (output == nullptr && window != nullptr && window->screen() != nullptr) {
        output = kapah::wlcompat::outputForScreen(window->screen());
    }

    const QByteArray ns = config.namespaceString.toUtf8();
    m_layerSurface = zwlr_layer_shell_v1_get_layer_surface(integration->layerShell(), m_surface,
        output, static_cast<zwlr_layer_shell_v1_layer>(config.layer), ns.constData());

    if (m_layerSurface == nullptr) {
        LOG_ERR(Wayland, "zwlr_layer_shell_v1_get_layer_surface returned null");
        return;
    }

    // The listener is a plain function pointer with no user data in the
    // generated API, so the surface pointer is the context.
    zwlr_layer_surface_v1_add_listener(m_layerSurface,
        [](void *data, zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t width,
            uint32_t height) {
            auto *self = static_cast<LayerSurface *>(data);
            if (self == nullptr || surface == nullptr) {
                return;
            }
            const bool first = !self->m_configured;
            self->m_configured = true;
            zwlr_layer_surface_v1_ack_configure(surface, serial);

            const int w = static_cast<int>(width);
            const int h = static_cast<int>(height);
            if (first) {
                Q_EMIT self->firstConfigure(w, h);
            } else {
                Q_EMIT self->resized(w, h);
            }
        },
        [](void *data, zwlr_layer_surface_v1 *surface) {
            Q_UNUSED(surface)
            auto *self = static_cast<LayerSurface *>(data);
            if (self == nullptr) {
                return;
            }
            LOG_WARN(Wayland, "compositor closed our layer surface");
            Q_EMIT self->closedByCompositor();
        },
        this);

    sendStateRequests();
}

LayerSurface::~LayerSurface()
{
    hideSurface();
}

void LayerSurface::sendStateRequests()
{
    if (m_layerSurface == nullptr) {
        return;
    }
    if (m_configured) {
        // The protocol forbids changing these after configure. Reaching here
        // means a caller tried; log rather than corrupt our state.
        LOG_WARN(Wayland, "attempted to change layer-surface state after configure");
    }

    // 0 x 0 means "let the compositor decide", which is what we want for the
    // bar's width. Height is always explicit.
    const auto w = static_cast<uint32_t>(m_config.width < 0 ? 0 : m_config.width);
    const auto h = static_cast<uint32_t>(m_config.height < 0 ? 0 : m_config.height);
    zwlr_layer_surface_v1_set_size(m_layerSurface, w, h);
    zwlr_layer_surface_v1_set_anchor(m_layerSurface,
        static_cast<zwlr_layer_surface_v1_anchor>(m_config.anchors));
    zwlr_layer_surface_v1_set_exclusive_zone(m_layerSurface, m_config.exclusiveZone);
    zwlr_layer_surface_v1_set_margin(m_layerSurface, m_config.margins.top(),
        m_config.margins.right(), m_config.margins.bottom(), m_config.margins.left());
    zwlr_layer_surface_v1_set_keyboard_interactivity(m_layerSurface,
        static_cast<zwlr_layer_surface_v1_keyboard_interactivity>(m_config.keyboard));
    zwlr_layer_surface_v1_set_layer(m_layerSurface,
        static_cast<zwlr_layer_shell_v1_layer>(m_config.layer));
}

void LayerSurface::applyConfig()
{
    sendStateRequests();
}

void LayerSurface::requestSize(int width, int height)
{
    if (m_layerSurface == nullptr) {
        return;
    }
    m_config.width = width;
    m_config.height = height;
    // set_size is explicitly legal after configure; the compositor answers with a
    // new configure if the size is not acceptable.
    zwlr_layer_surface_v1_set_size(m_layerSurface, static_cast<uint32_t>(qMax(0, width)),
        static_cast<uint32_t>(qMax(0, height)));
    if (m_surface != nullptr) {
        wl_surface_commit(m_surface);
    }
}

void LayerSurface::commitDamage()
{
    if (m_surface == nullptr) {
        return;
    }
    // Full-surface damage. Qt's raster backend hands us a QImage for the whole
    // surface; there is no cheaper honest option, and the bar is a thin strip.
    wl_surface_damage(m_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(m_surface);
}

void LayerSurface::showSurface()
{
    if (m_surface == nullptr) {
        return;
    }
    // Commit with no buffer so the compositor maps the surface immediately; Qt
    // attaches its own buffer on the next frame.
    wl_surface_commit(m_surface);
}

void LayerSurface::hideSurface()
{
    if (m_layerSurface != nullptr) {
        zwlr_layer_surface_v1_destroy(m_layerSurface);
        m_layerSurface = nullptr;
    }
    if (m_surface != nullptr) {
        wl_surface_destroy(m_surface);
        m_surface = nullptr;
    }
    m_configured = false;
}

void LayerSurface::handleConfigure(const QtWaylandClient::QWaylandSurfaceConfigure &configure)
{
    if (m_layerSurface == nullptr) {
        return;
    }
    m_configured = true;
    zwlr_layer_surface_v1_ack_configure(m_layerSurface, configure.serial);
}

// ---------------------------------------------------------------------------
// The QWaylandWindow subclass and its plugin
// ---------------------------------------------------------------------------

namespace {

class WaylandWindow : public QtWaylandClient::QWaylandWindow {
public:
    explicit WaylandWindow(QtWaylandClient::QWaylandWindow::Flags flags =
                               QtWaylandClient::QWaylandWindow::Flag::Default)
        : QtWaylandClient::QWaylandWindow(flags)
    {
    }

    void setInterface(QtWaylandClient::QWaylandShellInterface *interface) override
    {
        m_interface = interface;
    }

    QtWaylandClient::QWaylandWindow::Flags flags() const override
    {
        return QtWaylandClient::QWaylandWindow::flags()
            | QtWaylandClient::QWaylandWindow::Flag::Default;
    }

    void initWindow() override
    {
        QWindow *win = window();

        const LayerConfig *cfg = LayerShellRegistry::lookup(win);
        if (cfg != nullptr) {
            m_layerConfig = *cfg;
        } else {
            const LayerConfig *pending = LayerShellRegistry::consumePending();
            if (pending != nullptr) {
                m_layerConfig = *pending;
                LayerShellRegistry::noteFallback();
            }
        }

        if (!layerShellAvailable()) {
            LOG_ERR(Wayland,
                "no zwlr_layer_shell_v1 global: this shell cannot draw anything on a "
                "compositor that lacks it");
            return;
        }

        m_layerSurface = std::make_unique<LayerSurface>(win, m_layerConfig);
        if (m_layerSurface == nullptr) {
            return;
        }

        connect(m_layerSurface.get(), &LayerSurface::closedByCompositor, this,
            [this] { emit surfaceClosedByCompositor(); });

        connect(m_layerSurface.get(), &LayerSurface::firstConfigure, this,
            [this, win](int w, int h) {
                emit firstSurfaceConfigure(w, h);
                if (win != nullptr && (win->width() != w || win->height() != h)) {
                    win->resize(w, h);
                }
            });

        connect(m_layerSurface.get(), &LayerSurface::resized, this,
            [this, win](int w, int h) {
                emit surfaceResizedByCompositor(w, h);
                if (win != nullptr) {
                    win->resize(w, h);
                }
            });
    }

    void initWindowCopy() override
    {
        if (m_layerSurface) {
            m_layerSurface->requestSize(width(), height());
        }
        QtWaylandClient::QWaylandWindow::initWindowCopy();
    }

    void setVisible(bool visible) override
    {
        if (visible) {
            if (m_layerSurface) {
                m_layerSurface->showSurface();
            }
        } else if (m_layerSurface) {
            m_layerSurface->hideSurface();
        }
        QtWaylandClient::QWaylandWindow::setVisible(visible);
    }

Q_SIGNALS:
    void surfaceClosedByCompositor();
    void firstSurfaceConfigure(int width, int height);
    void surfaceResizedByCompositor(int width, int height);

private:
    QtWaylandClient::QWaylandShellInterface *m_interface = nullptr;
    LayerConfig m_layerConfig;
    std::unique_ptr<LayerSurface> m_layerSurface;
};

class WaylandWindowPlugin : public QtWaylandClient::QWaylandWindowPlugin {
public:
    QtWaylandClient::QWaylandWindow *create(
        QtWaylandClient::QWaylandIntegration *waylandIntegration,
        QtWaylandClient::QWaylandWindow::Flags flags) override
    {
        Q_UNUSED(waylandIntegration)
        return new WaylandWindow(flags);
    }

    bool supportsWindow(QtWaylandClient::QWaylandIntegration *waylandIntegration) const override
    {
        return waylandIntegration != nullptr && waylandIntegration->hasCapability(
            QtWaylandClient::QWaylandIntegration::Capability::LayerShell);
    }
};

bool g_pluginInstalled = false;

} // namespace

bool layerShellAvailable()
{
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr) {
        return false;
    }
    return integration->hasCapability(
        QtWaylandClient::QWaylandIntegration::Capability::LayerShell);
}

void installLayerShellPlugin()
{
    if (g_pluginInstalled) {
        return;
    }
    g_pluginInstalled = true;

    if (!layerShellAvailable()) {
        LOG_ERR(Wayland,
            "zwlr_layer_shell_v1 is not advertised by the compositor. kapah needs it; "
            "nothing will be visible. Is this running under niri?");
        return;
    }

    // Owning the plugin in a function-local static keeps it alive for the process
    // lifetime; registerPlugin() stores a raw pointer.
    static WaylandWindowPlugin *plugin = new WaylandWindowPlugin;
    QtWaylandClient::QWaylandWindow::registerPlugin(plugin);
}

// ---------------------------------------------------------------------------
// LayerShellWindow
// ---------------------------------------------------------------------------

LayerShellWindow::LayerShellWindow(const LayerConfig &config, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);

    // Push before touching windowHandle(): if handle creation eagerly builds the
    // platform window, the FIFO is what initWindow() will read.
    LayerShellRegistry::enqueuePending(m_config);

    // Frameless layer surfaces must not be decorated by Qt's own frame, and the
    // keyboard must not land here unless the config says so.
    if (QWindow *w = windowHandle()) {
        w->setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        w->setSurfaceType(QSurface::Window);
        if (m_config.keyboard == KeyboardInteractivity::None) {
            // NoFocus keeps Qt from requesting keyboard focus for the surface.
            w->setKeyboardGrabEnabled(false);
        }
        LayerShellRegistry::bind(w, m_config);
    }
}

LayerShellWindow::~LayerShellWindow()
{
    if (QWindow *w = windowHandle()) {
        LayerShellRegistry::unbind(w);
    }
}

void LayerShellWindow::setLayerConfig(const LayerConfig &config)
{
    const bool live = hasSurface();
    m_config = config;

    if (QWindow *w = windowHandle()) {
        LayerShellRegistry::bind(w, m_config);
    }

    // A live surface cannot change anchor/layer/zone after configure, so
    // applying this means recreating the platform window. Only when something
    // really differs -- callers use this from config reload, not hot paths.
    if (live) {
        const bool wasVisible = isVisible();
        if (QWindow *w = windowHandle()) {
            w->destroy();
        }
        if (wasVisible) {
            show();
            requestPaint();
        }
    }
}

void LayerShellWindow::setLayer(Layer layer)
{
    LayerConfig c = m_config;
    c.layer = layer;
    setLayerConfig(c);
}

void LayerShellWindow::setAnchors(Anchors anchors)
{
    LayerConfig c = m_config;
    c.anchors = anchors;
    setLayerConfig(c);
}

void LayerShellWindow::setExclusiveZone(int zone)
{
    LayerConfig c = m_config;
    c.exclusiveZone = zone;
    setLayerConfig(c);
}

void LayerShellWindow::setMargins(const QMargins &margins)
{
    LayerConfig c = m_config;
    c.margins = margins;
    setLayerConfig(c);
}

void LayerShellWindow::setKeyboardInteractivity(KeyboardInteractivity interactivity)
{
    LayerConfig c = m_config;
    c.keyboard = interactivity;
    if (interactivity == KeyboardInteractivity::Exclusive) {
        // An exclusive-keyboard surface needs to be able to take focus.
        setFocusPolicy(Qt::StrongFocus);
    }
    setLayerConfig(c);
}

void LayerShellWindow::setNamespaceString(const QString &ns)
{
    LayerConfig c = m_config;
    c.namespaceString = ns;
    setLayerConfig(c);
}

void LayerShellWindow::setOutputName(const QString &outputName)
{
    LayerConfig c = m_config;
    c.outputName = outputName;
    setLayerConfig(c);
}

void LayerShellWindow::showForScreen(QScreen *screen)
{
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
        if (screen == nullptr) {
            LOG_ERR(Wayland, "showForScreen() with no screens at all");
            return;
        }
    }

    QWindow *w = windowHandle();
    if (w == nullptr) {
        LOG_ERR(Wayland, "could not obtain a window handle for a layer surface");
        return;
    }

    // Screen assignment has to happen before the surface is created, because
    // zwlr_layer_shell_v1_get_layer_surface() takes the output.
    if (w->screen() != screen) {
        w->setScreen(screen);
    }
    if (m_config.outputName.isEmpty()) {
        m_config.outputName = screen->name();
        LayerShellRegistry::bind(w, m_config);
    }

    // A surface anchored to both horizontal edges spans the output; otherwise
    // the configured width applies. All in the output's logical pixels.
    const QRect geo = screen->geometry();
    const bool spans = m_config.anchors.testFlag(AnchorFlag::Left)
        && m_config.anchors.testFlag(AnchorFlag::Right);
    const int logicalWidth = spans ? geo.width() - m_config.margins.left() - m_config.margins.right()
                                   : m_config.width;
    resize(qMax(1, logicalWidth), qMax(1, m_config.height));

    if (!isVisible()) {
        show();
    }
    ensureSurface();
    requestPaint();
}

void LayerShellWindow::ensureSurface()
{
    if (m_creating) {
        return;
    }
    m_creating = true;

    QWindow *w = windowHandle();
    if (w == nullptr) {
        m_creating = false;
        return;
    }
    LayerShellRegistry::bind(w, m_config);

    // create() runs WaylandWindow::initWindow(), which is where the LayerSurface
    // is born. After this returns, hasSurface() is the truth.
    w->create();
    m_creating = false;

    if (hasSurface()) {
        Q_EMIT surfaceConfigured();
    }
}

bool LayerShellWindow::hasSurface() const
{
    const QWindow *w = windowHandle();
    return w != nullptr && w->handle() != nullptr;
}

void LayerShellWindow::releaseSurface()
{
    const bool wasVisible = isVisible();
    hide();
    if (QWindow *w = windowHandle()) {
        w->destroy();
    }
    if (wasVisible) {
        // Nothing to restore: the caller asked for release.
    }
}

void LayerShellWindow::requestPaint()
{
    QWindow *w = windowHandle();
    if (w != nullptr && w->handle() != nullptr) {
        // Qt's raster backend repaints the QWindow's backing store on update();
        // this is the single wakeup source for the whole shell.
        w->update();
    }
}

bool LayerShellWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::Show:
        if (QWindow *w = windowHandle()) {
            LayerShellRegistry::bind(w, m_config);
        }
        break;
    case QEvent::ScreenChangeInternal:
        // The window moved to another output; the layer surface must follow or be
        // recreated, because its output is fixed at creation.
        if (QWindow *w = windowHandle()) {
            if (w->screen() != nullptr) {
                m_config.outputName = w->screen()->name();
                LayerShellRegistry::bind(w, m_config);
            }
            w->destroy();
            if (isVisible()) {
                ensureSurface();
            }
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

WaylandCapabilities queryCapabilities()
{
    WaylandCapabilities caps;
    auto *integration = QtWaylandClient::QWaylandIntegration::instance();
    if (integration == nullptr) {
        return caps;
    }
    caps.layerShell = integration->hasCapability(
        QtWaylandClient::QWaylandIntegration::Capability::LayerShell);
    const auto globals = globalVersions();
    caps.layerShellVersion = globals.value(QStringLiteral("zwlr_layer_shell_v1"), 0);
    caps.sessionLock = globals.value(QStringLiteral("ext_session_lock_manager_v1"), 0) > 0;
    caps.idleNotify = globals.value(QStringLiteral("ext_idle_notifier_v1"), 0) > 0;
    caps.dataControl = globals.value(QStringLiteral("ext_data_control_manager_v1"), 0) > 0
        || globals.value(QStringLiteral("wlr_data_control_manager_v1"), 0) > 0;
    caps.gammaControl = globals.value(QStringLiteral("zwlr_gamma_control_manager_v1"), 0) > 0;
    caps.outputPower = globals.value(QStringLiteral("zwlr_output_power_manager_v1"), 0) > 0;
    return caps;
}

} // namespace kapah

#include "layersurface.moc"