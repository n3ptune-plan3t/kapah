#include "clockitem.h"

#include "calendar.h"
#include "popup.h"
#include "theme.h"
#include "widgets.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QScreen>
#include <QTime>

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace kapah {

ClockItem::ClockItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ClockItem::tick);
    armTimer();
    refreshText();
    setToolTip(QDateTime::currentDate().toString(QStringLiteral("dddd, MMMM d yyyy")));
}

ClockItem::~ClockItem()
{
    // The popup and its calendar are lazily destroyed; keep the parentage so
    // nothing leaks but the popup's surface is dropped via dismiss().
    if (m_popup != nullptr) {
        m_popup->dismiss();
    }
}

void ClockItem::armTimer()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime next = QDateTime(now.date(),
        QTime(now.time().hour(), now.time().minute(), 0)).addSecs(60);
    m_timer.start(qMax(1, now.msecsTo(next)));
}

void ClockItem::tick()
{
    refreshText();
    setToolTip(QDateTime::currentDate().toString(QStringLiteral("dddd, MMMM d yyyy")));
    armTimer();
}

void ClockItem::refreshText()
{
    m_label->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
}

void ClockItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_popup == nullptr) {
            m_popup = new Popup(ctx().theme, ctx().icons, QStringLiteral("kapah-calendar"), this);
            auto *vbox = new QVBoxLayout(m_popup);
            vbox->setContentsMargins(0, 0, 0, 0);
            m_calendar = new Calendar(ctx().theme, m_popup);
            vbox->addWidget(m_calendar);
            m_popup->setPlacement(Popup::Placement::BelowAnchor);
            connect(m_popup, &Popup::dismissRequested, m_popup, &Popup::dismiss);
        }
        m_calendar->resetToToday();
        m_popup->setAnchorWidget(this);
        m_popup->resizeToContent();
        m_popup->popup(output());
        event->accept();
    }
    QWidget::mousePressEvent(event);
}

} // namespace kapah
