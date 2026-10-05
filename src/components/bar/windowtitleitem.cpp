#include "windowtitleitem.h"

#include "state.h"
#include "widgets.h"

#include <QHBoxLayout>

namespace kapah {

WindowTitleItem::WindowTitleItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    m_label->setElideMode(Qt::ElideRight);
    layout->addWidget(m_label);

    connect(ctx.state, &niri::NiriState::focusedWindowChanged, this, &WindowTitleItem::refresh);
    connect(ctx.state, &niri::NiriState::windowsChanged, this, &WindowTitleItem::refresh);
    refresh();
}

void WindowTitleItem::refresh()
{
    if (ctx().state == nullptr) {
        m_label->setText(QString());
        return;
    }
    const niri::WindowInfo *w = ctx().state->focusedWindow();
    if (w == nullptr) {
        m_label->setText(QString());
        m_label->setDimmed(true);
    } else {
        m_label->setText(w->title.isEmpty() ? w->appId : w->title);
        m_label->setDimmed(false);
    }
}

} // namespace kapah
