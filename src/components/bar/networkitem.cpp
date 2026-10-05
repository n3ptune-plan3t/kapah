#include "networkitem.h"

#include "network.h"
#include "widgets.h"

#include <QHBoxLayout>

namespace kapah {

NetworkItem::NetworkItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    connect(ctx.network, &NetworkService::stateChanged, this, &NetworkItem::refresh);
    connect(ctx.network, &NetworkService::availableChanged, this, [this](bool) { refresh(); });
    refresh();
}

void NetworkItem::refresh()
{
    if (ctx().network == nullptr || !ctx().network->available()) {
        m_label->setText(QStringLiteral("offline"));
        m_label->setDimmed(true);
        return;
    }
    switch (ctx().network->state()) {
    case NetworkState::Connected:
        if (ctx().network->kind() == NetworkKind::Wireless) {
            m_label->setText(QStringLiteral("%1%").arg(ctx().network->signalStrength()));
            setToolTip(ctx().network->ssid());
        } else {
            m_label->setText(QStringLiteral("eth"));
            setToolTip(ctx().network->primaryInterface());
        }
        m_label->setDimmed(false);
        break;
    case NetworkState::Unknown:
        m_label->setText(QStringLiteral("…"));
        m_label->setDimmed(true);
        break;
    default:
        m_label->setText(QStringLiteral("offline"));
        m_label->setDimmed(true);
        break;
    }
}

} // namespace kapah
