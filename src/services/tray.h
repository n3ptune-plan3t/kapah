// StatusNotifierItem tray: watcher + host + minimal dbusmenu client.
//
// Roadmap Phase 3.8: "implement org.kde.StatusNotifierWatcher + host (or use a
// minimal in-house implementation) and render icons with dbusmenu support for
// context menus (implement a minimal com.canonical.dbusmenu client; lazy-fetch
// menu layout only when the menu is opened)".
//
// Decisions recorded in ADR-011 (tray):
//   * We implement BOTH roles. If another watcher already owns
//     org.kde.StatusNotifierWatcher we become a host instead of fighting for the
//     name; that is the only interoperable behaviour.
//   * Menus are fetched on open, never on hover. A tray item's layout can be
//     hundreds of items deep and fetching it eagerly would be the single largest
//     allocation event in the shell.
//   * The item list is capped (default 16). A pathological tray must not make
//     the bar wider than the screen.
//   * Activation modes: the SNI spec has three (click activates, or shows a menu
//     on either button). We honour them.

#pragma once

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include <functional>

class QDBusArgument;

class QDBusServiceWatcher;
class QDBusPendingCallWatcher;

namespace kapah {

class Theme;
class IconCache;

// One menu item, as far as the bar needs to know.
struct TrayMenuItem {
    QString label;
    QString iconName;
    QString iconThemePath;
    bool enabled = true;
    bool visible = true;
    bool isSeparator = false;
    bool isCheckable = false;
    bool isChecked = false;
    QStringList submenus;
    QVector<int> children;
    quint32 id = 0;
};

struct TrayMenu {
    quint32 id = 0;
    quint32 version = 0;
    quint32 status = 0;
    QVector<TrayMenuItem> items;
    bool valid() const { return id != 0; }
};

struct TrayItem {
    QString service;  // bus name
    QString path;     // object path
    QString id;       // app_id or fallback
    QString category;
    QString title;
    QString status;
    QString windowId;
    // Either a theme name or a raw ARGB32 blob from IconPixmap.
    QString iconName;
    QString iconThemePath;
    QByteArray iconPixmap;
    quint32 iconSize = 0;
    QString iconData;

    // NM state string: "Active" | "Passive" | "NeedsAttention".
    QString attention() const { return status; }
    bool needsAttention() const { return status == QLatin1String("NeedsAttention"); }

    // dbusmenu path, empty when the app has no menu.
    QString menuPath;
};

class TrayService : public QObject {
    Q_OBJECT
public:
    TrayService(Theme *theme, IconCache *icons, QObject *parent = nullptr);
    ~TrayService() override;

    // Takes the watcher name if it is free, otherwise becomes a host.
    void start();
    void stop();

    bool available() const { return m_available; }
    // "watcher" (we own the name) or "host" (someone else does) or "" (none).
    QString role() const { return m_role; }

    const QVector<TrayItem> &items() const { return m_items; }
    int maxItems() const { return m_maxItems; }
    void setMaxItems(int n) { m_maxItems = qBound(0, n, 64); }
    void setContextMenusEnabled(bool enabled) { m_menusEnabled = enabled; }
    void setMenuOnLeftClick(bool onLeft) { m_menuOnLeftClick = onLeft; }

    // --- activation -------------------------------------------------------
    // One of: "LeftClick", "MiddleClick", "RightClick", "DoubleClick", "Scroll".
    // Returns false when the item declares no such activation, in which case the
    // caller should show the menu instead.
    bool activate(const QString &busName, const QString &objectPath, const QString &mode);
    void scroll(const QString &busName, const QString &objectPath, int value,
        bool horizontal);
    // Right-click / context menu at a global position.
    void showContextMenu(const QString &busName, const QString &objectPath, const QPoint &globalPos);

    // Called when the user picks a menu entry: sends com.canonical.dbusmenu
    // Event with eventId "clicked".
    void invokeMenuItem(const QString &busName, const QString &menuPath, quint32 itemId,
        const QString &eventId);

Q_SIGNALS:
    void itemsChanged();
    void availableChanged(bool available);
    // An item wants a menu shown at a global position. The bar's tray module owns
    // the popup; the service only knows D-Bus.
    void contextMenuRequested(const QString &busName, const QString &menuPath, const QPoint &pos);

private:
    void becomeWatcher();
    void becomeHost();
    void registerHost();
    void registerItem(const QString &service, const QString &path);
    void unregisterItem(const QString &service, const QString &path);
    void refreshItem(const QString &service, const QString *pathPtr);
    void refreshItem(const QString &service, const QString &path)
    {
        refreshItem(service, &path);
    }
    void readItemProperties(const QString &service, const QString &path);
    void onItemPropertiesChanged(const QString &service, const QString &path,
        const QVariantMap &changed, const QVariantMap &invalidated);
public:
    // Fetches a dbusmenu layout. Lazy by design: called only when a menu is about
    // to be shown (roadmap Phase 3.8).
    void fetchMenu(const QString &busName, const QString *menuPathPtr,
        std::function<void(const TrayMenu &)> done);
    void fetchMenu(const QString &busName, const QString &menuPath,
        std::function<void(const TrayMenu &)> done)
    {
        fetchMenu(busName, &menuPath, std::move(done));
    }

private:
    void watchItemName(const QString &service);
    // Decodes a (ia{sv} &children) dbusmenu layout node.
    void parseLayout(const QDBusArgument &arg, TrayMenu &menu);

    Theme *m_theme = nullptr;
    IconCache *m_icons = nullptr;

    QVector<TrayItem> m_items;
    QHash<QString, QString> m_menuPaths; // "service:path" -> menu object path
    QHash<QString, QObject *> m_itemWatchers;
    QHash<QString, bool> m_menusEnabledByService;

    QString m_role;
    bool m_available = false;
    bool m_menusEnabled = true;
    bool m_menuOnLeftClick = false;
    int m_maxItems = 16;

    QDBusServiceWatcher *m_nameWatcher = nullptr;
    QString m_watcherService;
    QString m_watcherPath;
    QVector<QString> m_knownWatchers;
};

} // namespace kapah