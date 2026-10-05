#include "backlightitem.h"

#include "backlight.h"
#include "widgets.h"

#include <QWheelEvent>

#include <QHBoxLayout>

namespace kapah {

BacklightItem::BacklightItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    connect(ctx.backlight, &BacklightService::stateChanged, this, &BacklightItem::refresh);
    connect(ctx.backlight, &BacklightService::availableChanged, this, [this](bool) { refresh(); });
    refresh();
}

void BacklightItem::refresh()
{
    if (ctx().backlight == nullptr || !ctx().backlight->available()) {
        m_label->setText(QString());
        m_label->setDimmed(true);
        return;
    }
    m_label->setDimmed(false);
    m_label->setText(QStringLiteral("%1%").arg(ctx().backlight->percent()));
}

void BacklightItem::wheelEvent(QWheelEvent *event)
{
    if (ctx().backlight != nullptr && ctx().backlight->available()) {
        const int delta = event->angleDelta().y() > 0 ? ctx().backlight->step()
                                                      : -ctx().backlight->step();
        ctx().backlight->setPercent(ctx().backlight->percent() + delta);
    }
    event->accept();
}

} // namespace kapah
