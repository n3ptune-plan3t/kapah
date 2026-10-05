#include "icons.h"

#include "logging.h"
#include "paint.h"
#include "theme.h"

#include <QGuiApplication>
#include <QIcon>
#include <QIconEngine>
#include <QImageReader>
#include <QScreen>
#include <QStyle>
#include <QWindow>

namespace kapah {

namespace {

int dprToInt(qreal dpr)
{
    return qBound(250, static_cast<int>(dpr * 1000.0f + 0.5f), 8000);
}

qint64 pixmapBytes(const QPixmap &pm)
{
    if (pm.isNull()) {
        return 0;
    }
    // Shallow pixmaps reference a QImage; deep ones own the bits. Either way the
    // cost is width * height * 4 for a 32-bit surface.
    return static_cast<qint64>(pm.width()) * pm.height() * 4;
}

} // namespace

// ---------------------------------------------------------------------------
// Hash for the cache key
// ---------------------------------------------------------------------------

size_t qHash(const IconKey &k, size_t seed)
{
    // FNV-style mix of the string hash with the two integers, spread so that
    // entries differing only in size or dpr do not collide.
    return qHash(k.name, seed) ^ (static_cast<size_t>(k.size) << 3)
        ^ (static_cast<size_t>(k.dpr) << 17);
}

// ---------------------------------------------------------------------------
// IconCache
// ---------------------------------------------------------------------------

IconCache::IconCache(QObject *parent)
    : QObject(parent)
{
}

IconCache::~IconCache() = default;

void IconCache::setTheme(Theme *theme)
{
    m_theme = theme;
    if (m_theme != nullptr) {
        connect(m_theme, &Theme::themeChanged, this, &IconCache::invalidate,
            Qt::UniqueConnection);
    }
}

void IconCache::setMaxEntries(int entries)
{
    m_maxEntries = qBound(4, entries, 512);
    trim();
}

void IconCache::invalidate()
{
    m_entries.clear();
    m_dirty = true;
    Q_EMIT cacheInvalidated();
}

qint64 IconCache::estimatedBytes() const
{
    qint64 total = 0;
    for (const Entry &e : m_entries) {
        total += e.bytes;
    }
    return total;
}

void IconCache::trim()
{
    if (m_entries.size() <= m_maxEntries) {
        return;
    }
    // Evict least-recently-used until we are back under the cap. Sorting is fine:
    // this runs once per launcher close with 64 entries.
    QList<Key> keys = m_entries.keys();
    std::sort(keys.begin(), keys.end(), [this](const Key &a, const Key &b) {
        return m_entries.value(a).lastUse < m_entries.value(b).lastUse;
    });
    const int excess = m_entries.size() - m_maxEntries;
    for (int i = 0; i < excess; ++i) {
        m_entries.remove(keys.at(i));
    }
}

QPixmap IconCache::resolve(const QString &name, int size, int dpr, bool *found)
{
    *found = false;
    if (name.isEmpty()) {
        return {};
    }

    QIcon qicon = QIcon::fromTheme(name);
    if (qicon.isNull() || qicon.availableSizes().isEmpty()
        && qicon.pixmap(size, size).isNull()) {
        return {};
    }

    // Render once at exactly the size we will paint. This is the step that lets
    // us drop the QIcon immediately.
    QPixmap pm = qicon.pixmap(QSize(size, size), qreal(dpr) / 1000.0f, Qt::MatchBestFit);
    if (pm.isNull()) {
        return {};
    }
    *found = true;
    return pm;
}

QPixmap IconCache::icon(const QString &name, int size, qreal devicePixelRatio)
{
    if (name.isEmpty() || size <= 0) {
        return {};
    }
    const int dpr = dprToInt(devicePixelRatio);

    const IconKey key { name, size, dpr };
    ++m_clock;

    const auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        it->lastUse = m_clock;
        return it->pixmap;
    }

    bool found = false;
    QPixmap pm = resolve(name, size, dpr, &found);

    if (!found) {
        // Apply the ADR-004 fallback chain.
        for (const QString &fallback : iconFallbackNames(name)) {
            pm = resolve(fallback, size, dpr, &found);
            if (found) {
                break;
            }
        }
    }

    if (!found) {
        m_unresolved.insert(name, size);
        // A painted placeholder rather than an empty box, so a broken icon theme
        // looks obviously broken rather than like a shell bug.
        pm = QPixmap(size * dpr / 1000, size * dpr / 1000);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor c = m_theme != nullptr ? m_theme->color(QLatin1String(palette::Muted))
                                            : QColor(0x80, 0x80, 0x80);
        const QRectF r = pm.rect().adjusted(0.5, 0.5, -0.5, -0.5);
        strokeRounded(&p, r, qMin(4.0, r.width() / 4.0), c, 1.0);
        p.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.bottom()));
        p.end();
    }

    Entry entry;
    entry.pixmap = pm;
    entry.bytes = pixmapBytes(pm);
    entry.lastUse = m_clock;
    m_entries.insert(key, entry);

    if (m_entries.size() > m_maxEntries) {
        trim();
    }
    return pm;
}

QPixmap IconCache::iconFromFile(const QString &path, int size, qreal devicePixelRatio)
{
    if (path.isEmpty() || size <= 0) {
        return {};
    }
    const int dpr = dprToInt(devicePixelRatio);
    const IconKey key { path, size, dpr };
    ++m_clock;

    const auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        it->lastUse = m_clock;
        return it->pixmap;
    }

    QPixmap source(path);
    QPixmap pm;
    if (!source.isNull()) {
        // Downscale in one step: QPixmap::scaled is a smooth transform, which is
        // more than good enough and avoids loading every resolution variant of a
        // multi-size PNG.
        pm = source.scaled(size * dpr / 1000, size * dpr / 1000, Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
    }

    Entry entry;
    entry.pixmap = pm;
    entry.bytes = pixmapBytes(pm);
    entry.lastUse = m_clock;
    m_entries.insert(key, entry);
    if (m_entries.size() > m_maxEntries) {
        trim();
    }
    return pm;
}

void IconCache::paintIcon(QPainter *p, const QRectF &target, const QString &name, qreal dpr)
{
    if (p == nullptr || target.isEmpty()) {
        return;
    }
    // For the bar's one-shot paints, rendering straight into the target rect is
    // one allocation cheaper than a cached pixmap, so this path bypasses the
    // cache entirely when the target is bigger than the icon (no scaling).
    const int size = static_cast<int>(target.height() + 0.5);
    const int dprI = dprToInt(dpr);
    const IconKey key { name, size, dprI };
    const auto it = m_entries.constFind(key);
    if (it != m_entries.constEnd()) {
        p->drawPixmap(target.topLeft(), it->pixmap);
        return;
    }

    QIcon qicon = QIcon::fromTheme(name);
    if (!qicon.isNull()) {
        const QPixmap pm = qicon.pixmap(QSize(size, size), qreal(dprI) / 1000.0f,
            Qt::MatchBestFit);
        if (!pm.isNull()) {
            Entry entry;
            entry.pixmap = pm;
            entry.bytes = pixmapBytes(pm);
            entry.lastUse = ++m_clock;
            m_entries.insert(key, entry);
            p->drawPixmap(target.topLeft(), pm);
            return;
        }
    }
    // Last resort: reuse the cached placeholder path.
    const QPixmap pm = icon(name, size, dpr);
    if (!pm.isNull()) {
        p->drawPixmap(target.topLeft(), pm);
    }
}

// ---------------------------------------------------------------------------
// The ADR-004 fallback table
// ---------------------------------------------------------------------------

QStringList iconFallbackNames(const QString &purpose)
{
    static const QHash<QString, QStringList> table = {
        { QStringLiteral("audio-volume-high"),
            { QStringLiteral("audio-volume-high"), QStringLiteral("audio-volume-medium"),
                QStringLiteral("audio-volume-low"), QStringLiteral("preferences-system-sound") } },
        { QStringLiteral("audio-volume-medium"),
            { QStringLiteral("audio-volume-medium"), QStringLiteral("audio-volume-high"),
                QStringLiteral("audio-volume-low"), QStringLiteral("preferences-system-sound") } },
        { QStringLiteral("audio-volume-low"),
            { QStringLiteral("audio-volume-low"), QStringLiteral("audio-volume-medium"),
                QStringLiteral("audio-volume-high"), QStringLiteral("preferences-system-sound") } },
        { QStringLiteral("audio-volume-muted"),
            { QStringLiteral("audio-volume-muted"), QStringLiteral("audio-volume-low"),
                QStringLiteral("preferences-system-sound") } },
        { QStringLiteral("microphone"),
            { QStringLiteral("microphone"), QStringLiteral("audio-input-microphone"),
                QStringLiteral("audio-muted") } },
        { QStringLiteral("network-wired"),
            { QStringLiteral("network-wired"), QStringLiteral("network-wireless"),
                QStringLiteral("network-offline") } },
        { QStringLiteral("network-wireless"),
            { QStringLiteral("network-wireless"), QStringLiteral("network-wired"),
                QStringLiteral("network-mobile-broadband"), QStringLiteral("network-offline") } },
        { QStringLiteral("network-offline"),
            { QStringLiteral("network-offline"), QStringLiteral("network-wireless") } },
        { QStringLiteral("network-wireless-signal-excellent"),
            { QStringLiteral("network-wireless-signal-excellent"),
                QStringLiteral("network-wireless-signal-good"),
                QStringLiteral("network-wireless") } },
        { QStringLiteral("network-wireless-signal-good"),
            { QStringLiteral("network-wireless-signal-good"),
                QStringLiteral("network-wireless-signal-excellent"),
                QStringLiteral("network-wireless") } },
        { QStringLiteral("network-wireless-signal-ok"),
            { QStringLiteral("network-wireless-signal-ok"),
                QStringLiteral("network-wireless-signal-good"),
                QStringLiteral("network-wireless") } },
        { QStringLiteral("network-wireless-signal-weak"),
            { QStringLiteral("network-wireless-signal-weak"),
                QStringLiteral("network-wireless-signal-ok"),
                QStringLiteral("network-wireless") } },
        { QStringLiteral("network-wireless-signal-low"),
            { QStringLiteral("network-wireless-signal-low"),
                QStringLiteral("network-wireless-signal-weak"),
                QStringLiteral("network-wireless") } },
        { QStringLiteral("battery-full"),
            { QStringLiteral("battery-full"), QStringLiteral("battery-good"),
                QStringLiteral("battery-medium"), QStringLiteral("battery-low") } },
        { QStringLiteral("battery-good"),
            { QStringLiteral("battery-good"), QStringLiteral("battery-full"),
                QStringLiteral("battery-medium"), QStringLiteral("battery-low") } },
        { QStringLiteral("battery-medium"),
            { QStringLiteral("battery-medium"), QStringLiteral("battery-good"),
                QStringLiteral("battery-low"), QStringLiteral("battery-caution") } },
        { QStringLiteral("battery-low"),
            { QStringLiteral("battery-low"), QStringLiteral("battery-medium"),
                QStringLiteral("battery-caution"), QStringLiteral("battery-empty") } },
        { QStringLiteral("battery-caution"),
            { QStringLiteral("battery-caution"), QStringLiteral("battery-low"),
                QStringLiteral("battery-empty") } },
        { QStringLiteral("battery-empty"),
            { QStringLiteral("battery-empty"), QStringLiteral("battery-caution"),
                QStringLiteral("battery-missing") } },
        { QStringLiteral("battery-missing"),
            { QStringLiteral("battery-missing"), QStringLiteral("battery-full") } },
        { QStringLiteral("battery-full-charging"),
            { QStringLiteral("battery-full-charging"), QStringLiteral("battery-good-charging"),
                QStringLiteral("battery-full") } },
        { QStringLiteral("bluetooth"), { QStringLiteral("bluetooth"), QStringLiteral("preferences-bluetooth") } },
        { QStringLiteral("bluetooth-disabled"),
            { QStringLiteral("bluetooth-disabled"), QStringLiteral("bluetooth"),
                QStringLiteral("preferences-bluetooth") } },
        { QStringLiteral("video-display"),
            { QStringLiteral("video-display"), QStringLiteral("display-brightness-symbolic"),
                QStringLiteral("preferences-desktop-display") } },
        { QStringLiteral("system-shutdown"),
            { QStringLiteral("system-shutdown"), QStringLiteral("system-log-out") } },
        { QStringLiteral("system-lock-screen"),
            { QStringLiteral("system-lock-screen"), QStringLiteral("system-lock-screen-symbolic") } },
        { QStringLiteral("system-log-out"), { QStringLiteral("system-log-out"), QStringLiteral("system-shutdown") } },
        { QStringLiteral("system-suspend"),
            { QStringLiteral("system-suspend"), QStringLiteral("media-playback-pause") } },
        { QStringLiteral("preferences-system"), { QStringLiteral("preferences-system"), QStringLiteral("system-run") } },
        { QStringLiteral("edit-find"), { QStringLiteral("edit-find"), QStringLiteral("system-search"), QStringLiteral("accessories-calculator") } },
        { QStringLiteral("utilities-terminal"), { QStringLiteral("utilities-terminal"), QStringLiteral("terminal") } },
        { QStringLiteral("accessories-calculator"), { QStringLiteral("accessories-calculator"), QStringLiteral("calculator") } },
    };

    const auto it = table.constFind(purpose);
    if (it != table.constEnd()) {
        return it.value();
    }
    return { purpose };
}

QString iconNameForVolume(int percent, bool muted)
{
    if (muted || percent <= 0) {
        return QStringLiteral("audio-volume-muted");
    }
    if (percent >= 66) {
        return QStringLiteral("audio-volume-high");
    }
    if (percent >= 33) {
        return QStringLiteral("audio-volume-medium");
    }
    return QStringLiteral("audio-volume-low");
}

QString iconNameForMicrophone(bool muted)
{
    return muted ? QStringLiteral("audio-volume-muted") : QStringLiteral("microphone");
}

QString iconNameForNetwork(bool connected, bool wired, int signalPercent)
{
    if (!connected) {
        return QStringLiteral("network-offline");
    }
    if (wired || signalPercent <= 0) {
        return QStringLiteral("network-wired");
    }
    if (signalPercent >= 75) {
        return QStringLiteral("network-wireless-signal-excellent");
    }
    if (signalPercent >= 50) {
        return QStringLiteral("network-wireless-signal-good");
    }
    if (signalPercent >= 25) {
        return QStringLiteral("network-wireless-signal-ok");
    }
    return QStringLiteral("network-wireless-signal-weak");
}

QString iconNameForBattery(int percent, bool charging, bool present)
{
    if (!present) {
        return QStringLiteral("battery-missing");
    }
    static const QStringList normal = {
        QStringLiteral("battery-full"), QStringLiteral("battery-good"),
        QStringLiteral("battery-medium"), QStringLiteral("battery-low"),
    };
    static const QStringList chargingNames = {
        QStringLiteral("battery-full-charging"), QStringLiteral("battery-good-charging"),
        QStringLiteral("battery-medium-charging"), QStringLiteral("battery-low-charging"),
    };
    int band = 3;
    if (percent >= 90) {
        band = 0;
    } else if (percent >= 65) {
        band = 1;
    } else if (percent >= 35) {
        band = 2;
    }
    return charging ? chargingNames.at(band) : normal.at(band);
}

QString iconNameForBluetooth(bool on)
{
    return on ? QStringLiteral("bluetooth") : QStringLiteral("bluetooth-disabled");
}

QString iconNameForBrightness(int percent)
{
    Q_UNUSED(percent)
    return QStringLiteral("video-display");
}

QString iconNameForPower(const QString &action)
{
    return QStringLiteral("system-") + action;
}

QString iconNameForSession()
{
    return QStringLiteral("preferences-system");
}

QString iconNameForSearch()
{
    return QStringLiteral("edit-find");
}

QString iconNameForTerminal()
{
    return QStringLiteral("utilities-terminal");
}

QString iconNameForCalculator()
{
    return QStringLiteral("accessories-calculator");
}

} // namespace kapah