// A month-grid calendar, hand-painted.
//
// Roadmap Phase 3.3 requires "a lightweight calendar (custom painted month grid,
// no QCalendarWidget)". QCalendarWidget is a QMonthView inside a QCalendarView
// inside a QWidget, with a QAbstractItemModel, a navigation bar and a QPrinter-
// capable print path: several hundred kB of RSS for a grid of 42 cells.
//
// This is a widget that draws 42 cells and one header row. It owns no model, no
// delegates and no layout; the only state is which month is displayed.
//
// The clock's alignment matters more than anything else here (roadmap 4.1): the
// month advances from "today" rather than accumulating +30 days, so a month view
// never drifts across a shorter month.

#pragma once

#include <QDate>
#include <QWidget>

namespace kapah {

class IconCache;
class Theme;

class Calendar : public QWidget {
    Q_OBJECT
public:
    explicit Calendar(Theme *theme, QWidget *parent = nullptr);
    ~Calendar() override;

    // The month shown. Defaults to the current month.
    void setMonth(int year, int month);
    int year() const { return m_year; }
    int month() const { return m_month; }
    // Jumps back to the current month; used when the popup opens.
    void resetToToday();

    // The first weekday is Monday (ISO), which is what most of Europe uses and
    // what QLocale's default firstDayOfWeek() reports.
    void setFirstDayOfWeek(int dayOfWeek) { m_firstDay = qBound(1, dayOfWeek, 7); }

    QSize sizeHint() const override;

Q_SIGNALS:
    // Double-click or Enter on a day. We do not offer "jump to that date"
    // semantics because there is nothing to jump to; the launcher has its own
    // date handling if a user ever wants it.
    void dayActivated(const QDate &date);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void cellRect(int index, QRect *out) const;
    int indexForPosition(const QPoint &pos) const;
    QDate dateForIndex(int index) const;

    Theme *m_theme = nullptr;
    int m_year = 0;
    int m_month = 0;
    int m_firstDay = 1;
};

} // namespace kapah