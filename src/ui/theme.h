// Theme: one visual language, hand-painted, no QSS.
//
// Roadmap 4.2 forbids Qt style sheets in hot UI: parsing QSS at runtime is slow
// and memory hungry, and it fights a custom paintEvent. So a Theme is a plain
// struct of colours and metrics that widgets read while painting, plus a single
// themeChanged() signal that makes every widget re-polish.
//
// Visual direction (owner decision, 2026-10-03): flat, rounded (6-12 px), dark
// and light variants, one accent colour, no shadows or blur anywhere.

#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QMargins>
#include <QObject>
#include <QString>

namespace kapah {

enum class ThemeVariant {
    Dark,
    Light,
};

// Named colours. Every widget paints from these names, never from a literal, so
// a palette change is a one-line change here.
namespace palette {
inline constexpr const char *Background = "bg";
inline constexpr const char *Surface = "surface";
inline constexpr const char *SurfaceAlt = "surface-alt";
inline constexpr const char *Foreground = "fg";
inline constexpr const char *Muted = "muted";
inline constexpr const char *Accent = "accent";
inline constexpr const char *Urgent = "urgent";
inline constexpr const char *Warning = "warning";
inline constexpr const char *Positive = "positive";
inline constexpr const char *Border = "border";
inline constexpr const char *Hover = "hover";
inline constexpr const char *Active = "active";
inline constexpr const char *Focus = "focus";
inline constexpr const char *Shadow = "shadow";
} // namespace palette

// Metric scale. Everything is in device-independent pixels and gets multiplied
// by Theme::scale() at paint time, so a HiDPI screen and a 1.0 screen share one
// set of numbers.
struct Metrics {
    int spacing = 4;      // gap between adjacent items
    int spacingSmall = 2; // gap inside a pill
    int padding = 4;      // inner padding of a button
    int paddingLarge = 12;
    int radiusSmall = 4;
    int radius = 6;
    int radiusLarge = 12;
    int borderWidth = 1;
    int iconSmall = 16;
    int icon = 18;
    int iconLarge = 32;
    int hitSlop = 4; // extra clickable area around small targets
    int rowHeight = 28;
    int rowHeightLarge = 36;
    int scrollbarWidth = 6;
    int separatorWidth = 1;
};

class Theme : public QObject {
    Q_OBJECT
public:
    explicit Theme(QObject *parent = nullptr);
    ~Theme() override;

    // Loads from [general] scale/theme and [theme] from the config. Emits
    // themeChanged() only when something actually differs, so a config reload
    // that touches [bar] does not repolish the whole shell.
    void apply(const struct Config &config);

    // Explicit reload from a bare palette (the gallery binary).
    void setVariant(ThemeVariant variant);
    void setScale(double scale);
    void setColor(const QString &name, const QColor &color);
    void setFontFamily(const QString &family);

    ThemeVariant variant() const { return m_variant; }
    bool isDark() const { return m_variant == ThemeVariant::Dark; }
    double scale() const { return m_scale; }
    // Rounds and clamps, so painters can pass the result straight to QPainter.
    int px(int logical) const;

    const Metrics &metrics() const { return m_metrics; }
    const QFont &font(int role) const;
    const QFont &smallFont() const { return m_fonts.small; }
    const QFont &regularFont() const { return m_fonts.regular; }
    const QFont &mediumFont() const { return m_fonts.medium; }
    const QFont &boldFont() const { return m_fonts.bold; }
    const QFont &monoFont() const { return m_fonts.mono; }
    const QFont &iconFont() const { return m_fonts.icon; }

    const QColor &color(const QString &name) const;
    QColor withAlpha(const QString &name, int alpha) const;
    // Derives a colour that reads correctly on `background`.
    QColor contrastOn(const QColor &background) const;

    // The default palette for the current variant, used as the base before
    // config overrides are applied.
    static QHash<QString, QColor> defaultPalette(ThemeVariant variant);

Q_SIGNALS:
    void themeChanged();

private:
    void rebuildFonts();
    void rebuildMetrics();
    void rebuildPalette();

    ThemeVariant m_variant = ThemeVariant::Dark;
    double m_scale = 1.0;
    QString m_fontFamily;
    QHash<QString, QColor> m_palette;

    struct Fonts {
        QFont small;
        QFont regular;
        QFont medium;
        QFont bold;
        QFont mono;
        QFont icon;
        int iconPointSize = 0;
    } m_fonts;

    Metrics m_metrics;
    int m_baseBorderWidth = 1;
    bool m_opacity = true;

    // Fonts, one family and at most two weights (roadmap 4.2). QFontMetrics are
    // cached here rather than constructed per paint: constructing one costs about
    // 20 microseconds and a bar repaint does forty of them.
    mutable QHash<const QFont *, QFontMetrics> m_metricsCache;
};

} // namespace kapah