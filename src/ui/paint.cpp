#include "paint.h"

#include "theme.h"

#include <QtMath>

namespace kapah {

namespace {

// Rounded corner radii are clamped to half the shorter side; a radius larger than
// that produces self-intersecting paths and visibly broken output.
struct Radii {
    qreal tl;
    qreal tr;
    qreal br;
    qreal bl;
};

Radii clampRadii(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl)
{
    const qreal maxR = qMin(r.width(), r.height()) / 2.0;
    auto cl = [maxR](qreal v) { return qBound(qreal(0), v, maxR); };
    return Radii { cl(tl), cl(tr), cl(br), cl(bl) };
}

} // namespace

QPainterPath roundedPath(const QRectF &rect, qreal radius)
{
    return roundedPath(rect, radius, radius, radius, radius);
}

QPainterPath roundedPath(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl)
{
    const Radii rad = clampRadii(r, tl, tr, br, bl);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);

    // ArcTo with a degenerate rectangle draws a straight corner, which is exactly
    // what we want for a partially-rounded shape (a notification stack).
    path.arcTo(r.left(), r.top(), r.left() + rad.tl * 2, r.top() + rad.tl * 2, 180.0, -90.0);
    path.arcTo(r.right() - rad.tr * 2, r.top(), r.right(), r.top() + rad.tr * 2, 90.0, -90.0);
    path.arcTo(r.right() - rad.br * 2, r.bottom() - rad.br * 2, r.right(), r.bottom(), 0.0, -90.0);
    path.arcTo(r.left(), r.bottom() - rad.br * 2, r.left() + rad.bl * 2, r.bottom(), 270.0, -90.0);
    path.closeSubpath();
    return path;
}

void fillRounded(QPainter *p, const QRectF &rect, qreal radius, const QColor &fill)
{
    if (rect.isEmpty() || !fill.isValid()) {
        return;
    }
    // Fast path: a rectangle with no radius needs no AA at all, which is the
    // common case for the notification stack's interior fills.
    if (radius <= 0.5) {
        p->fillRect(rect, fill);
        return;
    }
    const bool prev = p->renderHints() & QPainter::Antialiasing;
    p->setRenderHint(QPainter::Antialiasing, true);
    p->fillPath(roundedPath(rect, radius), fill);
    p->setRenderHint(QPainter::Antialiasing, prev);
}

void strokeRounded(QPainter *p, const QRectF &rect, qreal radius, const QColor &stroke, qreal width)
{
    if (rect.isEmpty() || !stroke.isValid()) {
        return;
    }
    if (radius <= 0.5) {
        // Half-pixel inset so a 1 px line lands on whole device pixels.
        p->fillRect(rect.adjusted(0, 0, -width, -width), stroke);
        return;
    }
    const bool prev = p->renderHints() & QPainter::Antialiasing;
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF inset = rect.adjusted(width / 2.0, width / 2.0, -width / 2.0, -width / 2.0);
    p->strokePath(roundedPath(inset, qMax(0.0, radius - width / 2.0)), stroke);
    p->setRenderHint(QPainter::Antialiasing, prev);
}

void fillRoundedPartial(
    QPainter *p, const QRectF &rect, qreal tl, qreal tr, qreal br, qreal bl, const QColor &fill)
{
    if (rect.isEmpty() || !fill.isValid()) {
        return;
    }
    if (tl <= 0.5 && tr <= 0.5 && br <= 0.5 && bl <= 0.5) {
        p->fillRect(rect, fill);
        return;
    }
    const bool prev = p->renderHints() & QPainter::Antialiasing;
    p->setRenderHint(QPainter::Antialiasing, true);
    p->fillPath(roundedPath(rect, tl, tr, br, bl), fill);
    p->setRenderHint(QPainter::Antialiasing, prev);
}

void separatorLine(QPainter *p, const QRect &area, bool horizontal, const QColor &color)
{
    if (area.isEmpty() || !color.isValid()) {
        return;
    }
    // Deliberately not antialiased: a horizontal 1 px line must land on exactly
    // one device row.
    Q_UNUSED(p)
    if (horizontal) {
        const int y = area.center().y();
        p->fillRect(QRect(area.left(), y, area.width(), 1), color);
    } else {
        const int x = area.center().x();
        p->fillRect(QRect(x, area.top(), 1, area.height()), color);
    }
}

bool drawLevelBar(QPainter *p, const QRectF &rect, const QColor &track, const QColor &fill,
    double level, bool vertical)
{
    if (rect.isEmpty()) {
        return false;
    }
    level = qBound(0.0, level, 1.0);

    const qreal radius = qMin(rect.width(), rect.height()) / 2.0;
    fillRounded(p, rect, radius, track);
    if (level <= 0.0) {
        return true;
    }

    // Always leave a sliver so a 1% volume still shows something.
    const qreal extent = vertical ? rect.height() : rect.width();
    const qreal filled = qMax(qMin(extent, rect.height() >= 4.0 ? extent * level : 0.0), 2.0);

    QRectF fillRect = vertical ? QRectF(rect.left(), rect.bottom() - filled, rect.width(), filled)
                               : QRectF(rect.left(), rect.top(), filled, rect.height());
    fillRounded(p, fillRect, qMin(radius, filled / 2.0), fill);
    return true;
}

void drawBattery(QPainter *p, const QRectF &rect, double fraction, const QColor &outline,
    const QColor &fill, bool charging, bool warning)
{
    if (rect.isEmpty()) {
        return;
    }
    fraction = qBound(0.0, fraction, 1.0);

    // Body plus nub, drawn as paths rather than an icon so it works at any size
    // and needs no theme lookup.
    const qreal nubW = qMax(1.0, rect.width() * 0.12);
    const qreal bodyW = rect.width() - nubW - 1.0;
    const QRectF body(rect.left(), rect.top(), bodyW, rect.height());
    const qreal r = qMin(2.0, body.height() / 3.0);

    const bool prevAA = p->renderHints() & QPainter::Antialiasing;
    p->setRenderHint(QPainter::Antialiasing, true);

    const QColor bodyFill = fill.isValid() ? fill : outline;
    fillRounded(p, body.adjusted(0, 0, 0, 0), r, bodyFill);
    strokeRounded(p, body, r, outline, 1.0);

    // Nub.
    const qreal nubH = body.height() * 0.4;
    p->fillRect(QRectF(body.right() + 1.0, body.top() + (body.height() - nubH) / 2.0, nubW, nubH),
        outline);

    // Charge level inset from the body edge.
    if (fraction > 0.0) {
        const QRectF inner = body.adjusted(1.5, 1.5, -1.5, -1.5);
        const qreal fw = qMax(0.0, inner.width() * fraction);
        p->fillRect(QRectF(inner.left(), inner.top(), fw, inner.height()), bodyFill);
    }

    if (charging) {
        // A bolt: two triangles. Cheaper than an icon and always crisp.
        const QRectF bolt(inner.left() + inner.width() * 0.3, inner.top(),
            inner.width() * 0.4, inner.height());
        QPainterPath path;
        path.moveTo(bolt.center().x() + bolt.width() * 0.3, bolt.top());
        path.lineTo(bolt.center().x() - bolt.width() * 0.3, bolt.center().y());
        path.lineTo(bolt.center().x() + bolt.width() * 0.1, bolt.center().y());
        path.lineTo(bolt.center().x() - bolt.width() * 0.3, bolt.bottom());
        path.lineTo(bolt.center().x() + bolt.width() * 0.3, bolt.center().y() - bolt.height() * 0.1);
        path.closeSubpath();
        p->fillPath(path, bodyFill);
    }

    if (warning) {
        // A small exclamation badge, so a critical battery is not colour-only
        // (accessibility requirement, roadmap 10.3).
        const QRectF badge(body.right() - 7.0, body.top() - 3.0, 9.0, 9.0);
        p->fillPath(roundedPath(badge, 4.5), fill.isValid() ? fill : outline);
        p->setPen(outline);
        p->drawLine(QPointF(badge.center().x(), badge.top() + 2.5),
            QPointF(badge.center().x(), badge.center().y() + 0.5));
        p->drawPoint(QPointF(badge.center().x(), badge.bottom() - 2.0));
    }

    p->setRenderHint(QPainter::Antialiasing, prevAA);
}

void drawSignalBars(QPainter *p, const QRectF &rect, int activeOf4, const QColor &fill)
{
    if (rect.isEmpty()) {
        return;
    }
    activeOf4 = qBound(0, activeOf4, 4);
    const int gap = qMax(1, static_cast<int>(rect.width() * 0.15 / 4));
    const int barW = qMax(1, (static_cast<int>(rect.width()) - gap * 3) / 4);
    if (barW <= 0) {
        return;
    }
    const bool prevAA = p->renderHints() & QPainter::Antialiasing;
    p->setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < 4; ++i) {
        const qreal h = rect.height() * (0.35 + 0.65 * (i + 1) / 4.0);
        const QRectF bar(rect.left() + i * (barW + gap), rect.bottom() - h, barW, h);
        const bool on = i < activeOf4;
        p->fillPath(roundedPath(bar, qMin(1.5, barW / 2.0)),
            on ? fill : QColor(fill.red(), fill.green(), fill.blue(), 60));
    }
    p->setRenderHint(QPainter::Antialiasing, prevAA);
}

void drawChevron(QPainter *p, const QRectF &rect, const QColor &color, bool right)
{
    if (rect.isEmpty()) {
        return;
    }
    const QPen pen(color, qMax(1.0, rect.width() / 4.0), Qt::SolidLine, Qt::RoundCap,
        Qt::RoundJoin);
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(pen);
    const QPointF c = rect.center();
    const qreal d = rect.height() / 4.0;
    if (right) {
        p->drawLine(QPointF(c.x() - d * 0.5, c.y() - d), QPointF(c.x() + d * 0.5, c.y()));
        p->drawLine(QPointF(c.x() + d * 0.5, c.y()), QPointF(c.x() - d * 0.5, c.y() + d));
    } else {
        p->drawLine(QPointF(c.x() + d * 0.5, c.y() - d), QPointF(c.x() - d * 0.5, c.y()));
        p->drawLine(QPointF(c.x() - d * 0.5, c.y()), QPointF(c.x() + d * 0.5, c.y() + d));
    }
    p->restore();
}

void paintFlatShadow(QPainter *p, const QRectF &rect, qreal radius, int spread, int alpha)
{
    if (rect.isEmpty() || spread <= 0) {
        return;
    }
    // Four 1 px edge gradients instead of a blurred image: no allocation, no
    // filter setup. On a flat background this is visually indistinguishable from
    // a real shadow at these sizes.
    QColor c(0, 0, 0, qBound(0, alpha, 255));
    const int step = qMax(1, spread);
    p->save();
    p->setClipRect(rect.adjusted(-spread, -spread, spread, spread), Qt::IntersectClip);
    p->setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < spread; ++i) {
        const int a = c.alpha() * (spread - i) / spread;
        QColor edge = c;
        edge.setAlpha(a);
        const qreal t = static_cast<qreal>(i);
        p->fillRect(rect.left() - t, rect.top() - t, rect.width() + 2 * t, 1.0, edge);
        p->fillRect(rect.left() - t, rect.bottom() + t, rect.width() + 2 * t, 1.0, edge);
        p->fillRect(rect.left() - t, rect.top() - t, 1.0, rect.height() + 2 * t, edge);
        p->fillRect(rect.right() + t, rect.top() - t, 1.0, rect.height() + 2 * t, edge);
    }
    Q_UNUSED(step)
    p->restore();
}

QString elideText(const QString &text, int width, const QFontMetrics &fm, Qt::TextElideMode mode)
{
    if (width <= 0) {
        return {};
    }
    // horizontalAdvance() is the cheap test; elidedText() allocates.
    if (fm.horizontalAdvance(text) <= width) {
        return text;
    }
    return fm.elidedText(text, mode, width);
}

QColor lighten(const QColor &c, int amount)
{
    QColor out = c;
    out.setAlpha(255);
    const int target = qBound(0, c.lightness() + amount, 255);
    return QColor::fromHsvF(
        c.hsvHueF(), c.hsvSaturationF(), static_cast<float>(target) / 255.0f, 1.0f);
}

QColor darken(const QColor &c, int amount)
{
    return lighten(c, -amount);
}

QColor withAlpha(const QColor &c, int alpha)
{
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

int textBaseline(const QFontMetrics &fm, const QRect &area, Qt::Alignment align)
{
    const int y = area.center().y() + (fm.ascent() - fm.descent()) / 2;
    Q_UNUSED(align)
    return y;
}

} // namespace kapah