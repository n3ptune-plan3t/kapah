#include "batteryitem.h"

#include "logging.h"
#include "power.h"
#include "widgets.h"

#include <QHBoxLayout>

namespace kapah {

BatteryItem::BatteryItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    connect(ctx.power, &PowerService::stateChanged, this, &BatteryItem::refresh);
    connect(ctx.power, &PowerService::availableChanged, this, [this](bool) { refresh(); });
    refresh();
}

void BatteryItem::refresh()
{
    if (ctx().power == nullptr || !ctx().power->available()) {
        m_label->setText(QStringLiteral("—"));
        m_label->setDimmed(true);
        return;
    }
    if (!ctx().power->hasBattery()) {
        m_label->setText(QString());
        m_label->setDimmed(true);
        return;
    }
    const BatteryInfo &b = ctx().power->battery();
    QString text = b.percent >= 0 ? QString::number(b.percent) + QStringLiteral("%")
                                  : QStringLiteral("—");
    if (b.charging()) {
        text += QStringLiteral(" +");
    }
    m_label->setText(text);
    m_label->setDimmed(false);
    m_label->setColor(b.critical() ? QStringLiteral("urgent")
        : (b.low() ? QStringLiteral("warning") : QStringLiteral("fg")));

    // Once-per-discharge latches: forward as a notification exactly once.
    if (ctx().power->takeLowNotification()) {
        LOG_WARN(Power, "battery low");
    }
    if (ctx().power->takeCriticalNotification()) {
        LOG_WARN(Power, "battery critical");
    }
}

} // namespace kapah
