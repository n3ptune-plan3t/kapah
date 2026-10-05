#include "layoutitem.h"

#include "state.h"
#include "widgets.h"

#include <QHBoxLayout>

namespace kapah {

LayoutItem::LayoutItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    connect(ctx.state, &niri::NiriState::keyboardLayoutsChanged, this, &LayoutItem::refresh);
    connect(ctx.state, &niri::NiriState::keyboardLayoutSwitched, this, &LayoutItem::refresh);
    refresh();
}

void LayoutItem::refresh()
{
    if (ctx().state == nullptr) {
        m_label->setText(QString());
        return;
    }
    m_label->setText(ctx().state->keyboardLayouts().currentDisplayName());
}

} // namespace kapah
