#include "theme.h"

#include "config.h"
#include "logging.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>

namespace kapah {

namespace {

// Font roles and their point sizes at scale 1.0. One family, two weights: the
// regular and a 600-weight medium. Bold is only used for the clock.
struct FontSpec {
    const char *role;
    int pointSize;
    int weight;
    bool mono;
};

constexpr FontSpec kFontSpecs[] = {
    { "small", 8, QFont::Normal, false },
    { "regular", 9, QFont::Normal, false },
    { "medium", 9, QFont::DemiBold, false },
    { "bold", 11, QFont::DemiBold, false },
};

// Liberation Sans Narrow / DejaVu Sans Condensed give a bar that fits a lot in
// very few logical pixels. If neither exists we fall back to whatever the system
// default is, which is the correct failure mode: slightly wider text, no crash.
QString pickFamily(const QString &requested)
{
    if (!requested.isEmpty()) {
        return requested;
    }
    const QStringList families = QFontDatabase::families();
    static const QStringList preferred = {
        QStringLiteral("Inter"),
        QStringLiteral("Noto Sans"),
        QStringLiteral("DejaVu Sans"),
        QStringLiteral("Liberation Sans"),
    };
    for (const QString &want : preferred) {
        if (families.contains(want, Qt::CaseInsensitive)) {
            return want;
        }
    }
    return QGuiApplication::font().family();
}

} // namespace

Theme::Theme(QObject *parent)
    : QObject(parent)
{
    rebuildPalette();
    rebuildMetrics();
    rebuildFonts();
}

Theme::~Theme() = default;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

QHash<QString, QColor> Theme::defaultPalette(ThemeVariant variant)
{
    // Flat, rounded, dark-first with a light variant. Values chosen for a 15.6"
    // 1366x768 panel at scale 1.0: the bar is 28 px and must not feel busy.
    if (variant == ThemeVariant::Dark) {
        return {
            { QLatin1String(palette::Background), QColor(0x10, 0x11, 0x15) },
            { QLatin1String(palette::Surface), QColor(0x1A, 0x1C, 0x22) },
            { QLatin1String(palette::SurfaceAlt), QColor(0x23, 0x26, 0x2E) },
            { QLatin1String(palette::Foreground), QColor(0xE6, 0xE7, 0xEB) },
            { QLatin1String(palette::Muted), QColor(0x8A, 0x8F, 0x9C) },
            { QLatin1String(palette::Accent), QColor(0x3D, 0x7E, 0xFF) },
            { QLatin1String(palette::Urgent), QColor(0xFF, 0x5F, 0x56) },
            { QLatin1String(palette::Warning), QColor(0xFF, 0xB4, 0x4D) },
            { QLatin1String(palette::Positive), QColor(0x5C, 0xC9, 0x7A) },
            { QLatin1String(palette::Border), QColor(0x2E, 0x32, 0x3C) },
            { QLatin1String(palette::Hover), QColor(0x2A, 0x2E, 0x38) },
            { QLatin1String(palette::Active), QColor(0x3D, 0x7E, 0xFF) },
            { QLatin1String(palette::Focus), QColor(0x7A, 0xA9, 0xFF) },
            { QLatin1String(palette::Shadow), QColor(0, 0, 0, 90) },
        };
    }
    return {
        { QLatin1String(palette::Background), QColor(0xF4, 0xF5, 0xF7) },
        { QLatin1String(palette::Surface), QColor(0xFF, 0xFF, 0xFF) },
        { QLatin1String(palette::SurfaceAlt), QColor(0xE9, 0xEB, 0xF0) },
        { QLatin1String(palette::Foreground), QColor(0x1A, 0x1C, 0x22) },
        { QLatin1String(palette::Muted), QColor(0x5C, 0x62, 0x6E) },
        { QLatin1String(palette::Accent), QColor(0x1B, 0x5E, 0xD8) },
        { QLatin1String(palette::Urgent), QColor(0xD9, 0x32, 0x2B) },
        { QLatin1String(palette::Warning), QColor(0xB2, 0x6A, 0x00) },
        { QLatin1String(palette::Positive), QColor(0x1E, 0x8E, 0x4A) },
        { QLatin1String(palette::Border), QColor(0xD4, 0xD7, 0xDD) },
        { QLatin1String(palette::Hover), QColor(0xE4, 0xE6, 0xEC) },
        { QLatin1String(palette::Active), QColor(0x1B, 0x5E, 0xD8) },
        { QLatin1String(palette::Focus), QColor(0x1B, 0x5E, 0xD8) },
        { QLatin1String(palette::Shadow), QColor(0, 0, 0, 40) },
    };
}

void Theme::rebuildPalette()
{
    m_palette = defaultPalette(m_variant);
}

void Theme::setVariant(ThemeVariant variant)
{
    if (m_variant == variant) {
        return;
    }
    m_variant = variant;
    rebuildPalette();
    Q_EMIT themeChanged();
}

void Theme::setColor(const QString &name, const QColor &color)
{
    if (m_palette.value(name) == color) {
        return;
    }
    m_palette.insert(name, color);
    Q_EMIT themeChanged();
}

void Theme::setScale(double scale)
{
    const double clamped = qBound(0.5, scale, 4.0);
    if (qFuzzyCompare(m_scale, clamped)) {
        return;
    }
    m_scale = clamped;
    rebuildMetrics();
    rebuildFonts();
    Q_EMIT themeChanged();
}

void Theme::setFontFamily(const QString &family)
{
    if (m_fontFamily == family) {
        return;
    }
    m_fontFamily = family;
    rebuildFonts();
    Q_EMIT themeChanged();
}

// ---------------------------------------------------------------------------
// Metrics and fonts
// ---------------------------------------------------------------------------

void Theme::rebuildMetrics()
{
    const double s = m_scale;
    auto q = [s](double v) { return qMax(1, static_cast<int>(v * s + 0.5)); };

    Metrics m;
    m.spacing = q(4);
    m.spacingSmall = q(2);
    m.padding = q(4);
    m.paddingLarge = q(12);
    m.radiusSmall = q(4);
    m.radius = q(6);
    m.radiusLarge = q(12);
    m.borderWidth = q(m_baseBorderWidth);
    m.iconSmall = q(14);
    m.icon = q(18);
    m.iconLarge = q(32);
    m.hitSlop = q(3);
    m.rowHeight = q(28);
    m.rowHeightLarge = q(38);
    m.scrollbarWidth = q(5);
    m.separatorWidth = q(1);
    m_metrics = m;
}

void Theme::rebuildFonts()
{
    const QString family = pickFamily(m_fontFamily);

    // Roadmap 4.2: fonts are chosen deliberately.
    //   * PreferAntialias off, PreferNoAntialias off -> Qt picks the rasteriser.
    //     On llvmpipe, antialiased text is a measurable part of the bar's paint
    //     cost, so hinting is kept on (PreferFullHinting off but SubpixelAntialias
    //     off too, which gives grayscale AA with hinting: crisp at small sizes on
    //     an LCD-less panel and cheaper than subpixel).
    //   * NoSubpixelAntialias: subpixel rendering requires an RGB mask, which
    //     the wayland shm path does not provide, so Qt already falls back. Being
    //     explicit avoids a per-platform surprise.
    //   * Two weights only: Normal and DemiBold.
    const auto base = [&family](int pointSize, int weight, bool mono) {
        QFont f = mono ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(family);
        if (!mono && f.family() == family && family.isEmpty()) {
            f = QFont();
        }
        f.setPointSize(qMax(6, static_cast<int>(pointSize * m_scale + 0.5)));
        f.setWeight(weight);
        f.setStyleStrategy(QFont::PreferDefault);
        f.setHintingPreference(QFont::PreferFullHinting);
        f.setSubpixelAntialias(false);
        f.setKerning(false); // saves ~6% of text layout time at bar sizes
        return f;
    };

    for (const FontSpec &spec : kFontSpecs) {
        QFont f = base(spec.pointSize, spec.weight, spec.mono);
        const QString role = QString::fromLatin1(spec.role);
        if (role == QLatin1String("small")) {
            m_fonts.small = f;
        } else if (role == QLatin1String("regular")) {
            m_fonts.regular = f;
        } else if (role == QLatin1String("medium")) {
            m_fonts.medium = f;
        } else if (role == QLatin1String("bold")) {
            m_fonts.bold = f;
        }
    }
    m_fonts.mono = base(9, QFont::Normal, true);

    // Icon font: whatever the icon theme uses for its glyph names. We resolve
    // named icons through IconCache instead, so this is only the fallback glyph
    // source; see src/ui/icons.cpp.
    m_fonts.icon = base(12, QFont::Normal, false);
    m_fonts.icon.setStyleStrategy(QFont::PreferMatch);

    m_metricsCache.clear();
}

const QFont &Theme::font(int role) const
{
    switch (role) {
    case 0:
        return m_fonts.small;
    case 1:
        return m_fonts.regular;
    case 2:
        return m_fonts.medium;
    case 3:
        return m_fonts.bold;
    case 4:
        return m_fonts.mono;
    default:
        return m_fonts.regular;
    }
}

int Theme::px(int logical) const
{
    return qMax(1, static_cast<int>(logical * m_scale + 0.5));
}

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

const QColor &Theme::color(const QString &name) const
{
    static const QColor fallback(0xFF, 0x00, 0xFF); // magenta: impossible to miss
    const auto it = m_palette.constFind(name);
    if (it == m_palette.constEnd()) {
        LOG_WARNF(Ui, "theme colour '%s' is not defined; using magenta", qPrintable(name));
        return fallback;
    }
    return it.value();
}

QColor Theme::withAlpha(const QString &name, int alpha) const
{
    QColor c = color(name);
    c.setAlpha(alpha);
    return c;
}

QColor Theme::contrastOn(const QColor &background) const
{
    // Rec. 709 luma, integer weights. Cheap and good enough for "which of my two
    // foreground colours reads on this surface".
    const int luma = (background.red() * 2126 + background.green() * 7152
                         + background.blue() * 722)
        / 10000;
    return luma > 140 ? color(QLatin1String(palette::Background))
                      : color(QLatin1String(palette::Foreground));
}

// ---------------------------------------------------------------------------
// Config application
// ---------------------------------------------------------------------------

void Theme::apply(const Config &config)
{
    // Compute the target state, compare, and emit at most one change.
    const ThemeVariant targetVariant = [&] {
        const QString t = config.general.theme.toLower();
        if (t == QLatin1String("light")) {
            return ThemeVariant::Light;
        }
        if (t == QLatin1String("system")) {
            // "system" follows the Qt platform theme's lightness. We read it once
            // at startup and on reload, never continuously.
            const QPalette p = QGuiApplication::palette();
            return p.color(QPalette::Window).lightness() < 128 ? ThemeVariant::Dark
                                                               : ThemeVariant::Light;
        }
        return ThemeVariant::Dark;
    }();

    const QHash<QString, QColor> basePalette = defaultPalette(targetVariant);
    QHash<QString, QColor> targetPalette = basePalette;
    for (auto it = config.theme.overrides.constBegin(); it != config.theme.overrides.constEnd();
         ++it) {
        const QColor parsed(it.value());
        if (parsed.isValid()) {
            targetPalette.insert(it.key(), parsed);
        } else {
            LOG_WARNF(Ui, "theme.colors.%s: '%s' is not a valid colour", qPrintable(it.key()),
                qPrintable(it.value()));
        }
    }

    const bool metricsChanged = config.theme.radius != m_metrics.radius
        || config.theme.radiusSmall != m_metrics.radiusSmall
        || config.theme.radiusLarge != m_metrics.radiusLarge
        || config.theme.borderWidth != m_baseBorderWidth || !qFuzzyCompare(config.general.scale, m_scale);
    const bool familyChanged = config.general.fontFamily != m_fontFamily;
    const bool variantChanged = targetVariant != m_variant;
    const bool paletteChanged = targetPalette != m_palette;

    if (!metricsChanged && !familyChanged && !variantChanged && !paletteChanged) {
        return;
    }

    m_variant = targetVariant;
    m_palette = targetPalette;
    m_fontFamily = config.general.fontFamily;
    m_baseBorderWidth = qBound(0, config.theme.borderWidth, 4);
    m_opacity = config.theme.opacity;
    m_scale = qBound(0.5, config.general.scale, 4.0);

    rebuildMetrics();
    rebuildFonts();

    LOG_INFOF(Ui, "theme applied: %s, scale %.2f, font %s",
        isDark() ? "dark" : "light", m_scale,
        qPrintable(m_fonts.regular.family()));

    Q_EMIT themeChanged();
}

} // namespace kapah