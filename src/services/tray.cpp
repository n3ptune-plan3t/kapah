#include "tray.h"

#include "icons.h"
#include "logging.h"
#include "theme.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QPoint>

namespace kapah {

namespace {

const char *kWatcherService = "org.kde.StatusNotifierWatcher";
const char *kWatcherPath = "/StatusNotifierWatcher";
const char *kWatcherInterface = "org.kde.StatusNotifierWatcher";
const char *kItemInterface = "org.kde.StatusNotifierItem";
const char *kHostInterface = "org.kde.StatusNotifierHost";
const char *kMenuInterface = "com.canonical.dbusmenu";
const char *kProps = "org.freedesktop.DBus.Properties";

struct ActivationMode {
    const char *name;
    quint32 value;
};

// SNI's ActivationToken enum.
constexpr ActivationMode kActivationModes[] = {
    { "LeftClick", 0 },
    { "MiddleClick", 1 },
    { "RightClick", 2 },
    { "Scroll", 3 },
    { "DoubleClick", 4 },
};

bool activationSupported(const QString &token, const QString &mode)
{
    for (const ActivationMode &m : kActivationModes) {
        if (mode != QLatin1String(m.name)) {
            continue;
        }
        // The token is the OR of the modes the app handles.
        const QStringList parts = token.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            if (p.trimmed() == QLatin1String(m.name)) {
                return true;
            }
        }
    }
    return false;
}

QByteArray decodeIconPixmap(const QVariant &v, quint32 *sizeOut)
{
    // IconPixmap is a(iiiiay): width, height, bytesPerLine, and an ARGB32 array.
    // Qt marshals it as a QDBusArgument of (ii, ii, ii, QByteArray) in practice;
    // we accept both that and the documented tuple.
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentSignature().startsWith(QLatin1String("(iiiiay)"))) {
        int width = 0;
        int height = 0;
        arg.beginArray();
        arg >> width >> height;
        int bytesPerLine = 0;
        int dummy = 0;
        arg >> bytesPerLine;
        arg >> dummy;
        QByteArray data;
        arg >> data;
        arg.endArray();
        if (sizeOut != nullptr) {
            *sizeOut = static_cast<quint32>(qMax(width, height));
        }
        return data;
    }
    return {};
}

} // namespace

TrayService::TrayService(Theme *theme, IconCache *icons, QObject *parent)
    : QObject(parent)
    , m_theme(theme)
    , m_icons(icons)
{
    qDBusRegisterMetaType<QVariantMap>();
}

TrayService::~TrayService() = default;

void TrayService::start()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        LOG_WARN(Tray, "no session bus: the tray module is unavailable");
        return;
    }

    m_watcherService = QString::fromLatin1(kWatcherService);
    m_watcherPath = QString::fromLatin1(kWatcherPath);

    // If somebody else already owns the watcher name, we are a host. There is no
    // third option and no reason to try to take the name away from a working
    // watcher.
    if (bus.interface()->isServiceRegistered(m_watcherService.toUtf8()).value()) {
        becomeHost();
    } else {
        becomeWatcher();
    }

    m_available = m_role.isEmpty() ? false : true;
    Q_EMIT availableChanged(m_available);
}

void TrayService::stop()
{
    for (TrayItem &item : m_items) {
        if (!item.service.isEmpty()) {
            auto bus = QDBusConnection::sessionBus();
            bus.unregisterService(item.service);
            bus.unregisterObject(item.path);
        }
    }
    m_items.clear();

    if (m_role == QLatin1String("watcher")) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(m_watcherService);
    }
    m_role.clear();
    m_available = false;
    Q_EMIT availableChanged(false);
}

// ---------------------------------------------------------------------------
// Watcher role
// ---------------------------------------------------------------------------

void TrayService::becomeWatcher()
{
    auto bus = QDBusConnection::sessionBus();

    if (!bus.registerService(m_watcherService)) {
        LOG_WARN(Tray,
            "another process took org.kde.StatusNotifierWatcher between our check and our "
            "register; becoming a host instead");
        becomeHost();
        return;
    }
    bus.registerObject(m_watcherPath, this, QDBusConnection::ExportScriptableSlots
            | QDBusConnection::ExportScriptableSignals);
    m_role = QStringLiteral("watcher");

    // Watch for a *second* watcher appearing: if one does, we must stop being one,
    // because two watchers would both hold items and neither would be authoritative.
    m_nameWatcher = new QDBusServiceWatcher(m_watcherService, bus,
        QDBusServiceWatcher::WatchForRegistration, this);
    connect(m_nameWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
        LOG_WARN(Tray, "a second StatusNotifierWatcher registered; kapah steps down to a host");
        auto bus2 = QDBusConnection::sessionBus();
        bus2.unregisterService(m_watcherService);
        bus2.unregisterObject(m_watcherPath);
        becomeHost();
    });

    LOG_INFO(Tray, "owning org.kde.StatusNotifierWatcher");
}

// ---------------------------------------------------------------------------
// Host role
// ---------------------------------------------------------------------------

void TrayService::becomeHost()
{
    m_role = QStringLiteral("host");
    auto bus = QDBusConnection::sessionBus();
    bus.registerObject(QStringLiteral("/StatusNotifierHost"), this,
        QDBusConnection::ExportScriptableSlots);
    registerHost();

    // Connect to the watcher interface's signals so we learn about items as they
    // appear. The watcher's interface is discovered, not assumed.
    m_nameWatcher = new QDBusServiceWatcher(m_watcherService, bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_nameWatcher, &QDBusServiceWatcher::serviceRegistered, this,
        &TrayService::registerHost);

    LOG_INFO(Tray, "acting as StatusNotifierHost for the existing watcher");
}

void TrayService::registerHost()
{
    auto bus = QDBusConnection::sessionBus();
    // Introspect the watcher for its interface name; it is almost always
    // org.kde.StatusNotifierWatcher but some implementations differ.
    QDBusMessage introspect = QDBusMessage::createMethodCall(m_watcherService, m_watcherPath,
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    QDBusMessage reply = bus.call(introspect, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        return;
    }
    const QString xml = reply.arguments().isEmpty() ? QString()
                                                    : reply.arguments().first().toString();
    const QString iface = xml.contains(QStringLiteral("org.kde.StatusNotifierWatcher"))
        ? m_watcherInterface
        : QStringLiteral("org.freedesktop.StatusNotifierWatcher");

    bus.connect(m_watcherService, m_watcherPath, iface,
        QStringLiteral("StatusNotifierItemRegistered"), this,
        [this](const QString &service) {
            // The signal carries the service; the path is always
            // /StatusNotifierItem on the SNI convention.
            registerItem(service, QStringLiteral("/StatusNotifierItem"));
        });
    bus.connect(m_watcherService, m_watcherPath, iface,
        QStringLiteral("StatusNotifierItemUnregistered"), this,
        [this](const QString &service, const QString &path) { unregisterItem(service, path); });

    // The watcher's RegisteredStatusNotifierItems property gives us the current
    // set. One read at startup; after that we are driven by the two signals.
    QDBusMessage get = QDBusMessage::createMethodCall(m_watcherService, m_watcherPath,
        QString::fromLatin1(kProps), QStringLiteral("Get"));
    get.setArguments({ iface, QStringLiteral("RegisteredStatusNotifierItems") });
    QDBusMessage r2 = bus.call(get, QDBus::Block, 2000);
    if (r2.type() != QDBusMessage::ReplyMessage) {
        return;
    }
    const QStringList registered = r2.arguments().isEmpty() ? QStringList()
                                                           : r2.arguments().first().toStringList();
    for (const QString &entry : registered) {
        const int slash = entry.indexOf(QLatin1Char('/'), 1);
        if (slash <= 0) {
            continue;
        }
        registerItem(entry.left(slash), entry.mid(slash));
    }
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

void TrayService::registerItem(const QString &service, const QString &path)
{
    if (m_items.size() >= m_maxItems) {
        LOG_WARNF(Tray, "refusing a %dth tray item; the cap is %d (raise [tray] max-items)",
            m_items.size(), m_maxItems);
        return;
    }
    for (const TrayItem &existing : m_items) {
        if (existing.service == service && existing.path == path) {
            return;
        }
    }

    TrayItem item;
    item.service = service;
    item.path = path;
    m_items.append(item);

    readItemProperties(service, path);
    watchItemName(service);
    Q_EMIT itemsChanged();
}

void TrayService::unregisterItem(const QString &service, const QString &path)
{
    const qsizetype before = m_items.size();
    m_items.removeIf([&](const TrayItem &i) { return i.service == service && i.path == path; });
    if (m_items.size() != before) {
        auto it = m_itemWatchers.find(service);
        if (it != m_itemWatchers.end()) {
            delete it.value();
            m_itemWatchers.erase(it);
        }
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(service);
        bus.unregisterObject(path);
        Q_EMIT itemsChanged();
    }
}

void TrayService::watchItemName(const QString &service)
{
    if (m_itemWatchers.contains(service)) {
        return;
    }
    auto *w = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(),
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(w, &QDBusServiceWatcher::serviceUnregistered, this,
        [this, service] { unregisterItem(service, QStringLiteral("/StatusNotifierItem")); });
    m_itemWatchers.insert(service, w);
}

void TrayService::readItemProperties(const QString &service, const QString &path)
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(service, path, QString::fromLatin1(kProps),
        QStringLiteral("GetAll"));
    msg.setArguments({ QString::fromLatin1(kItemInterface) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, service, path](QDBusPendingCallReply<QVariantMap> reply) {
            w->deleteLater();
            if (reply.isError()) {
                LOG_WARNF(Tray, "cannot read SNI properties from %s%s: %s", qPrintable(service),
                    qPrintable(path), qPrintable(reply.error().message()));
                unregisterItem(service, path);
                return;
            }
            onItemPropertiesChanged(service, path, reply.value(), {});
        });
}

void TrayService::onItemPropertiesChanged(const QString &service, const QString &path,
    const QVariantMap &changed, const QVariantMap &invalidated)
{
    Q_UNUSED(invalidated)
    for (TrayItem &item : m_items) {
        if (item.service != service || item.path != path) {
            continue;
        }

        auto apply = [&changed, &item](const char *key, QString &target) {
            const auto it = changed.constFind(QString::fromLatin1(key));
            if (it != changed.constEnd()) {
                target = it.value().toString();
            }
        };
        apply("Category", item.category);
        apply("Id", item.id);
        apply("Title", item.title);
        apply("Status", item.status);
        apply("WindowId", item.windowId);
        apply("IconName", item.iconName);
        apply("IconThemePath", item.iconThemePath);

        const auto iconIt = changed.constFind(QStringLiteral("IconPixmap"));
        if (iconIt != changed.constEnd()) {
            quint32 sz = 0;
            const QByteArray data = decodeIconPixmap(iconIt.value(), &sz);
            if (!data.isEmpty()) {
                item.iconPixmap = data;
                item.iconSize = sz;
            }
        }

        const auto menuIt = changed.constFind(QStringLiteral("Menu"));
        if (menuIt != changed.constEnd()) {
            item.menuPath = menuIt.value().value<QDBusObjectPath>().path();
            if (!item.menuPath.isEmpty()) {
                m_menuPaths.insert(service + QLatin1Char(':') + path, item.menuPath);
            }
        }

        // The bar needs an accessible name even when the app sets no Title.
        if (item.title.isEmpty()) {
            item.title = item.id;
        }
        Q_EMIT itemsChanged();
        return;
    }
}

void TrayService::refreshItem(const QString &service, const QString *pathPtr)
{
    readItemProperties(service, pathPtr != nullptr ? *pathPtr : QString());
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

bool TrayService::activate(const QString &busName, const QString &objectPath, const QString &mode)
{
    // Read the item's supported modes first so we do not invoke an action the app
    // does not handle (which the spec says is a protocol error). Both hops are
    // async: this runs on the wheel-and-click path and must never block the UI
    // thread on a busy or wedged tray app.
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(busName, objectPath,
        QString::fromLatin1(kProps), QStringLiteral("Get"));
    msg.setArguments({ QString::fromLatin1(kItemInterface), QStringLiteral("ActivatedModes") });
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 1500), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, busName, objectPath, mode](QDBusPendingCallReply<QVariant> reply) {
            w->deleteLater();
            if (reply.isError()) {
                return;
            }
            const QString token = reply.value().toString();
            if (!activationSupported(token, mode)) {
                return;
            }

            // The SNI spec defines exactly one activation entry point:
            // Activate(x, y). Wheel and middle click are mapped to the same
            // call; there is no separate DoubleClick method to invoke.
            QDBusMessage act = QDBusMessage::createMethodCall(busName, objectPath,
                QString::fromLatin1(kItemInterface), QStringLiteral("Activate"));
            act.setArguments({ 0, 0 });
            auto bus2 = QDBusConnection::sessionBus();
            auto *aw = new QDBusPendingCallWatcher(bus2.asyncCall(act, 2000), this);
            connect(aw, &QDBusPendingCallWatcher::finished, this,
                [aw](QDBusPendingCallReply<void> ar) {
                    aw->deleteLater();
                    Q_UNUSED(ar)
                });
        });
    return true;
}

void TrayService::scroll(const QString &busName, const QString &objectPath, int value,
    bool horizontal)
{
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(busName, objectPath,
        QString::fromLatin1(kItemInterface), QStringLiteral("Scroll"));
    msg.setArguments({ static_cast<int>(value), QString::fromLatin1(horizontal ? "horizontal"
                                                                              : "vertical") });
    bus.call(msg, QDBus::Block, 1500);
}

void TrayService::showContextMenu(const QString &busName, const QString &objectPath,
    const QPoint &globalPos)
{
    const QString menuPath = m_menuPaths.value(busName + QLatin1Char(':') + objectPath);
    if (menuPath.isEmpty() || !m_menusEnabled) {
        // No menu: fall back to Activate, which is what the spec says an app with
        // no menu expects from a right click.
        if (!activate(busName, objectPath, QStringLiteral("RightClick"))) {
            activate(busName, objectPath, QStringLiteral("LeftClick"));
        }
        return;
    }

    // Force the item's status to Active before showing the menu: several apps
    // refuse to render a menu otherwise.
    QDBusMessage statusMsg = QDBusMessage::createMethodCall(busName, objectPath,
        QString::fromLatin1(kProps), QStringLiteral("Set"));
    statusMsg.setArguments({ QString::fromLatin1(kItemInterface), QStringLiteral("Status"),
        QVariant(QStringLiteral("Active")) });
    QDBusConnection::sessionBus().call(statusMsg, QDBus::Block, 1000);

    Q_EMIT contextMenuRequested(busName, menuPath, globalPos);
}

void TrayService::invokeMenuItem(const QString &busName, const QString &menuPath, quint32 itemId,
    const QString &eventId)
{
    if (!m_menusEnabled) {
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(busName, menuPath,
        QString::fromLatin1(kMenuInterface), QStringLiteral("Event"));
    QVariantMap event;
    event.insert(QStringLiteral("type"), eventId);
    event.insert(QStringLiteral("id"), static_cast<int>(itemId));
    event.insert(QStringLiteral("timestamp"), 0);
    msg.setArguments({ QVariant::fromValue(event), 0u });
    bus.call(msg, QDBus::Block, 2000);
}

// ---------------------------------------------------------------------------
// com.canonical.dbusmenu, fetched on demand only
// ---------------------------------------------------------------------------

void TrayService::fetchMenu(const QString &busName, const QString *menuPathPtr,
    std::function<void(const TrayMenu &)> done)
{
    const QString menuPath = menuPathPtr != nullptr ? *menuPathPtr : QString();
    if (!m_menusEnabled || menuPath.isEmpty()) {
        if (done) {
            done(TrayMenu {});
        }
        return;
    }

    auto bus = QDBusConnection::sessionBus();
    QDBusMessage props = QDBusMessage::createMethodCall(busName, menuPath,
        QString::fromLatin1(kProps), QStringLiteral("GetAll"));
    props.setArguments({ QString::fromLatin1(kMenuInterface) });

    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(props, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
        [this, w, busName, menuPath, done](QDBusPendingCallReply<QVariantMap> reply) {
            w->deleteLater();
            TrayMenu menu;
            if (reply.isError()) {
                LOG_WARNF(Tray, "dbusmenu GetAll on %s failed: %s", qPrintable(menuPath),
                    qPrintable(reply.error().message()));
                if (done) {
                    done(menu);
                }
                return;
            }
            const QVariantMap p = reply.value();
            menu.id = p.value(QStringLiteral("Version"), 0u).toUInt();
            menu.version = p.value(QStringLiteral("Version"), 0u).toUInt();
            menu.status = p.value(QStringLiteral("Status"), 0u).toUInt();

            QDBusMessage layout = QDBusMessage::createMethodCall(busName, menuPath,
                QString::fromLatin1(kMenuInterface), QStringLiteral("GetLayout"));
            layout.setArguments({ 0u, 0u });

            auto bus2 = QDBusConnection::sessionBus();
            auto *lw = new QDBusPendingCallWatcher(bus2.asyncCall(layout, 3000), this);
            connect(lw, &QDBusPendingCallWatcher::finished, this,
                [this, lw, done](QDBusPendingCallReply<uint, uint, QDBusArgument> lr) {
                    lw->deleteLater();
                    if (lr.isError()) {
                        LOG_WARNF(Tray, "dbusmenu GetLayout failed: %s",
                            qPrintable(lr.error().message()));
                    } else {
                        parseLayout(lr.value().third, menu);
                    }
                    if (done) {
                        done(menu);
                    }
                });
        });
}

void TrayService::parseLayout(const QDBusArgument &arg, TrayMenu &menu)
{
    // Layout signature: (u revision, i parentId, (ia{sv} &children) layout)
    quint32 revision = 0;
    int parentId = 0;
    arg >> revision >> parentId;
    const auto node = qdbus_cast<QVariantMap>(arg).value();
    const QVariantMap properties = node.value(QStringLiteral("properties")).toMap();

    TrayMenuItem item;
    item.id = properties.value(QStringLiteral("id"), 0u).toUInt();
    item.label = properties.value(QStringLiteral("label")).toString();
    item.enabled = properties.value(QStringLiteral("enabled"), true).toBool();
    item.visible = properties.value(QStringLiteral("visible"), true).toBool();
    item.isSeparator = properties.value(QStringLiteral("type"))
                           .toString()
                           == QLatin1String("separator");
    item.isCheckable = properties.value(QStringLiteral("toggle-type")).toString()
        == QLatin1String("checkmark");
    item.isChecked = properties.value(QStringLiteral("toggle-state"), 0u).toUInt() != 0;

    const QVariant icon = properties.value(QStringLiteral("icon-name"));
    if (icon.isValid()) {
        item.iconName = icon.toString();
    }

    const QVariantMap children = node.value(QStringLiteral("children")).toMap();
    if (!children.isEmpty()) {
        // A submenu: we render it as a flat entry that opens a nested popup, and
        // the ids are enough to fetch its layout on demand.
        for (auto it = children.constBegin(); it != children.constEnd(); ++it) {
            item.children.append(it.key().toInt());
        }
        if (!item.label.isEmpty()) {
            item.submenus.append(item.label);
        }
    }

    if (!item.isSeparator || !item.label.isEmpty()) {
        menu.items.append(item);
    }
}

} // namespace kapah