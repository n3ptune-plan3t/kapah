// Painting helpers. Every custom widget paints through these so the visual
// language stays consistent and so the anti-aliasing policy is decided once.
//
// Anti-aliasing policy (roadmap 4.2 + ADR-011):
//   * Rounded corners: antialiased. They are small and few (a bar has maybe
//     three), and without AA they look broken.
//   * Straight lines and separators: NOT antialiased. A 1 px separator through
//     an AA'd painter lands on a half-pixel and renders as a 2 px grey smear.
//   * Fills of large areas: not antialiased (no curved edges).
//   * Text: grayscale antialiasing with hinting (see Theme::rebuildFonts).

#pragma once

#include <QColor>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QRectF>

namespace kapah {

class Theme;

// Rounded-rect helper that avoids the AA cost for rectangular requests.
void fillRounded(QPainter *p, const QRectF &rect, qreal radius, const QColor &fill);
void strokeRounded(QPainter *p, const QRectF &rect, qreal radius, const QColor &stroke,
    qreal width = 1.0);
// Fills with a per-corner-radius variant used by the launcher rows (top corners
// rounded, bottom square) and by notification stacks.
void fillRoundedPartial(
    QPainter *p, const QRectF &rect, qreal tl, qreal tr, qreal br, qreal bl, const QColor &fill);

// 1 px separator at an exact device pixel. `horizontal` picks the axis.
void separatorLine(QPainter *p, const QRect &area, bool horizontal, const QColor &color);

// Horizontal level meter used by the volume and brightness modules and the OSD.
// Returns false when nothing was drawn.
bool drawLevelBar(QPainter *p, const QRectF &rect, const QColor &track, const QColor &fill,
    double level, bool vertical = false);

// Battery glyph: a rounded body plus a nub, filled proportionally. `charging`
// draws a bolt. Cheaper and crisper than an icon lookup.
void drawBattery(QPainter *p, const QRectF &rect, double fraction, const QColor &outline,
    const QColor &fill, bool charging, bool warning);

// Signal-strength glyph: four bars of increasing height.
void drawSignalBars(QPainter *p, const QRectF &rect, int activeOf4, const QColor &fill);

// Simple chevron for disclosure and for the "play" triangle.
void drawChevron(QPainter *p, const QRectF &rect, const QColor &color, bool right);

// A soft drop shadow, painted manually into the destination. Wayland has no
// protocol for server-side shadows and Qt's QGraphicsDropShadowEffect needs
// QtWidgets' effect machinery plus a full-screen intermediate buffer. One
// cheap pass over a pre-blurred rect is cheaper than both.
void paintFlatShadow(QPainter *p, const QRectF &rect, qreal radius, int spread, int alpha);

// Clips text to `width`, appending an ellipsis. Uses a cached QFontMetrics from
// the theme so a bar repaint does not allocate a metrics object per label.
QString elideText(const QString &text, int width, const QFontMetrics &fm, Qt::TextElideMode mode);

// Inverts a colour's lightness, preserving alpha. Used for hover states instead
// of a second palette entry.
QColor lighten(const QColor &c, int amount);
QColor darken(const QColor &c, int amount);
QColor withAlpha(const QColor &c, int alpha);

// Composites `src` over `dst` in premultiplied space without allocating.
inline QColor blend(const QColor &dst, const QColor &src, int srcAlpha)
{
    const int a = qBound(0, srcAlpha, 255);
    if (a == 0) {
        return dst;
    }
    if (a == 255) {
        QColor c = src;
        c.setAlpha(255);
        return c;
    }
    const int inv = 255 - a;
    return QColor((src.red() * a + dst.red() * inv) / 255,
        (src.green() * a + dst.green() * inv) / 255,
        (src.blue() * a + dst.blue() * inv) / 255, 255);
}

// Renders text at a sub-pixel-consistent baseline. Callers should use
// fontMetrics().height()/2 + fontMetrics().ascent() for the y.
int textBaseline(const QFontMetrics &fm, const QRect &area, Qt::Alignment align);

// Builds a QPainterPath for a rounded rectangle. Exposed because several widgets
// need the path for clipping, not just filling.
QPainterPath roundedPath(const QRectF &rect, qreal radius);
// Per-corner variant: tl, tr, br, bl. A zero radius on a corner makes it square,
// which is what the notification stack and launcher rows want.
QPainterPath roundedPath(const QRectF &rect, qreal tl, qreal tr, qreal br, qreal bl);

} // namespace kapah