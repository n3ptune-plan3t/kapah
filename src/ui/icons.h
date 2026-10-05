// Icon resolution and caching (ADR-004).
//
// The important half of that ADR is not "don't link Qt6::Svg", it is "do not
// retain a QIcon". A retained QIcon holds a QIconLoaderEngine that keeps every
// size variant the source file provides; for a 512x512 symbolic pack that is
// megabytes of resident memory for something we draw at 16 px. So:
//
//   resolve name -> QIcon -> render once at exactly (size, dpr) -> QPixmap -> drop
//
// The cache is an LRU of 64 entries (roadmap 4.3). Icon pixmaps are cheap to
// recreate (a PNG decode of a 48x48 file is well under a millisecond) and only
// matter on a hot path like launcher scrolling.

#pragma once

#include <QHash>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace kapah {

class Theme;

// The cache key lives at namespace scope because qHash() must be a free
// function and a nested private type cannot be named from one.
struct IconKey {
    QString name;
    int size = 0;
    // Device pixel ratio stored scaled by 1000 so it is exactly representable as
    // an int: 1.0 -> 1000, 1.5 -> 1500, 2.0 -> 2000.
    int dpr = 1000;
    bool operator==(const IconKey &o) const
    {
        return size == o.size && dpr == o.dpr && name == o.name;
    }
};
size_t qHash(const IconKey &key, size_t seed = 0);

class IconCache : public QObject {
    Q_OBJECT
public:
    explicit IconCache(QObject *parent = nullptr);
    ~IconCache() override;

    void setTheme(Theme *theme);
    void setMaxEntries(int entries);

    // Themed icon by freedesktop name, with the ADR-004 fallback chain applied.
    // Returns a null QPixmap only when even the placeholder cannot be built.
    QPixmap icon(const QString &name, int size, qreal devicePixelRatio = 1.0);

    // Absolute path. Used for notification image-path hints and for .desktop
    // Icon= entries that are paths rather than names.
    QPixmap iconFromFile(const QString &path, int size, qreal devicePixelRatio = 1.0);

    // Paints straight into the destination rect without an intermediate pixmap.
    // Saves one allocation for one-shot paints (bar modules, tray items).
    void paintIcon(QPainter *p, const QRectF &target, const QString &name, qreal dpr = 1.0);

    // Drops everything. Called on theme and DPR changes.
    void invalidate();
    // Drops only entries that are not currently pinned. Called when the launcher
    // closes so the tray and bar icons stay cached.
    void trim();

    int entryCount() const { return m_entries.size(); }
    qint64 estimatedBytes() const;

    // Diagnostic: names that could not be resolved and fell back. Reported by
    // `kapahctl status` so a broken icon theme is visible instead of mysterious.
    QStringList unresolvedNames() const { return m_unresolved.values(); }

Q_SIGNALS:
    void cacheInvalidated();

private:
    QPixmap resolve(const QString &name, int size, int dpr, bool *found);

    struct Entry {
        QPixmap pixmap;
        qint64 bytes = 0;
        quint64 lastUse = 0;
    };
    QHash<IconKey, Entry> m_entries;
    QHash<QString, int> m_unresolved;
    Theme *m_theme = nullptr;
    int m_maxEntries = 64;
    quint64 m_clock = 0;
    bool m_dirty = true;
};

// ---------------------------------------------------------------------------
// The freedesktop lookup table from ADR-004.
// ---------------------------------------------------------------------------

// Returns the list of names to try, in order, for a semantic purpose. Exposed so
// the gallery can render the whole table and so the fallback logic is testable.
QStringList iconFallbackNames(const QString &purpose);

// Convenience wrappers for the semantic purposes the shell uses. Each returns
// the first name that resolves.
QString iconNameForVolume(int percent, bool muted);
QString iconNameForMicrophone(bool muted);
QString iconNameForNetwork(bool connected, bool wired, int signalPercent);
QString iconNameForBattery(int percent, bool charging, bool present);
QString iconNameForBluetooth(bool on);
QString iconNameForBrightness(int percent);
QString iconNameForPower(const QString &action);
QString iconNameForSession();
QString iconNameForSearch();
QString iconNameForTerminal();
QString iconNameForCalculator();

} // namespace kapah
