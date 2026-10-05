#include "calendar.h"

#include "paint.h"
#include "theme.h"

#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>

namespace kapah {

namespace {
constexpr int kWeeksShown = 6;
constexpr int kWeekdays = 7;
} // namespace

Calendar::Calendar(Theme *theme, QWidget *parent)
    : QWidget(parent)
    , m_theme(theme)
{
    m_firstDay = QLocale().firstDayOfWeek();
    const QDate today = QDate::currentDate();
    m_year = today.year();
    m_month = today.month();
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
}

Calendar::~Calendar() = default;

void Calendar::setMonth(int year, int month)
{
    year = qBound(1, year, 9999);
    month = qBound(1, month, 12);
    if (m_year == year && m_month == month) {
        return;
    }
    m_year = year;
    m_month = month;
    update();
}

void Calendar::resetToToday()
{
    const QDate today = QDate::currentDate();
    setMonth(today.year(), today.month());
}

QSize Calendar::sizeHint() const
{
    if (m_theme == nullptr) {
        return QSize(224, 176);
    }
    const Metrics &m = m_theme->metrics();
    const int cell = m_theme->px(m.rowHeight);
    const int header = m_theme->px(m.rowHeight);
    return QSize(cell * kWeekdays + m_theme->px(m.paddingLarge) * 2,
        header + cell * kWeeksShown + m_theme->px(m.paddingLarge) * 2);
}

void Calendar::cellRect(int index, QRect *out) const
{
    if (m_theme == nullptr || index < 0 || index >= kWeekdays * kWeeksShown) {
        return;
    }
    const Metrics &m = m_theme->metrics();
    const int pad = m_theme->px(m.paddingLarge);
    const int cell = m_theme->px(m.rowHeight);
    const int header = m_theme->px(m.rowHeight);
    const int col = index % kWeekdays;
    const int row = index / kWeekdays;
    *out = QRect(pad + col * cell, pad + header + row * cell, cell, cell);
}

QDate Calendar::dateForIndex(int index) const
{
    // The grid always starts at the Monday (or locale first day) on or before
    // the 1st of the displayed month, and covers six weeks so the height does not
    // change when the user navigates.
    const QDate first(m_year, m_month, 1);
    const int firstOffset = (first.dayOfWeek() - m_firstDay + kWeekdays) % kWeekdays;
    const QDate start = first.addDays(-firstOffset);
    return start.addDays(index);
}

int Calendar::indexForPosition(const QPoint &pos) const
{
    if (m_theme == nullptr) {
        return -1;
    }
    const Metrics &m = m_theme->metrics();
    const int pad = m_theme->px(m.paddingLarge);
    const int cell = m_theme->px(m.rowHeight);
    const int header = m_theme->px(m.rowHeight);
    const int x = pos.x() - pad;
    const int y = pos.y() - pad - header;
    if (x < 0 || y < 0) {
        return -1;
    }
    const int col = x / cell;
    const int row = y / cell;
    if (col >= kWeekdays || row >= kWeeksShown) {
        return -1;
    }
    return row * kWeekdays + col;
}

void Calendar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    if (m_theme == nullptr) {
        return;
    }
    QPainter p(this);
    const Metrics &m = m_theme->metrics();
    const int pad = m_theme->px(m.paddingLarge);
    const int cell = m_theme->px(m.rowHeight);
    const int header = m_theme->px(m.rowHeight);
    const QDate today = QDate::currentDate();

    // --- header: month name, centred
    p.setFont(m_theme->mediumFont());
    p.setPen(m_theme->color(QLatin1String(palette::Foreground)));
    const QString monthName = QLocale().standaloneMonthName(m_month) + QLatin1Char(' ')
        + QString::number(m_year);
    p.drawText(QRect(pad, pad, width() - pad * 2, header),
        Qt::AlignCenter | Qt::TextSingleLine, monthName);

    // --- weekday initials
    const QLocale locale;
    p.setFont(m_theme->smallFont());
    const QFontMetrics smallFm(p.font());
    p.setPen(m_theme->color(QLatin1String(palette::Muted)));
    for (int i = 0; i < kWeekdays; ++i) {
        const int dayOfWeek = ((m_firstDay - 1 + i) % kWeekdays) + 1;
        const QString initial = locale.dayName(dayOfWeek, QLocale::ShortFormat).left(1);
        const QRect r(pad + i * cell, pad + header - smallFm.height(), cell, smallFm.height());
        p.drawText(r, Qt::AlignCenter | Qt::TextSingleLine, initial);
    }

    // --- days
    p.setFont(m_theme->smallFont());
    const QFontMetrics fm(p.font());
    const QColor fg = m_theme->color(QLatin1String(palette::Foreground));
    const QColor muted = m_theme->color(QLatin1String(palette::Muted));
    const QColor accent = m_theme->color(QLatin1String(palette::Accent));

    for (int i = 0; i < kWeekdays * kWeeksShown; ++i) {
        QRect r;
        cellRect(i, &r);
        const QDate d = dateForIndex(i);

        if (!d.month() == !m_month) {
            continue;
        }
        const bool inMonth = d.month() == m_month && d.year() == m_year;
        const bool isToday = d == today;
        const bool isWeekend = d.dayOfWeek() == Qt::Saturday || d.dayOfWeek() == Qt::Sunday;

        if (isToday) {
            // A filled circle plus white text. Not colour-only: the ring is a
            // shape change as well as a colour change.
            const qreal radius = qMin(r.width(), r.height()) / 2.0 - m_theme->px(3);
            const QRectF dot(r.center().x() - radius, r.center().y() - radius, radius * 2,
                radius * 2);
            p.setRenderHint(QPainter::Antialiasing, true);
            fillRounded(&p, dot, radius, accent);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(m_theme->contrastOn(accent));
        } else {
            p.setPen(inMonth ? (isWeekend ? muted : fg) : m_theme->withAlpha(
                QLatin1String(palette::Muted), 110));
        }

        p.drawText(r, Qt::AlignCenter | Qt::TextSingleLine, QString::number(d.day()));
        Q_UNUSED(fm)
    }
}

void Calendar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int index = indexForPosition(event->position().toPoint());
    if (index < 0) {
        return;
    }
    const QDate d = dateForIndex(index);
    if (d.month() == m_month && d.year() == m_year) {
        Q_EMIT dayActivated(d);
    }
    event->accept();
}

void Calendar::mouseDoubleClickEvent(QMouseEvent *event)
{
    mousePressEvent(event);
}

} // namespace kapah