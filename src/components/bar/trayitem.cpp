#include "trayitem.h"

#include "logging.h"
#include "tray.h"
#include "widgets.h"

#include <QMouseEvent>

#include <QHBoxLayout>

namespace kapah {

TrayItemView::TrayItemView(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ctx.theme->px(2));

    connect(ctx.tray, &TrayService::itemsChanged, this, &TrayItemView::refresh);
    connect(ctx.tray, &TrayService::availableChanged, this, [this](bool) { refresh(); });
    refresh();
}

void TrayItemView::refresh()
{
    if (ctx().tray == nullptr || !ctx().tray->available()) {
        // No tray application registered: render nothing (zero width).
        setVisible(false);
        return;
    }
    setVisible(true);
    auto *layout = qobject_cast<QHBoxLayout *>(this->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const TrayItem &item : ctx().tray->items()) {
        auto *button = new IconButton(ctx().theme, ctx().icons, this);
        button->setShape(IconButton::Shape::Plain);
        button->setIconName(item.iconName.isEmpty() ? QStringLiteral("application-x-executable")
                                                    : item.iconName);
        button->setToolTip(item.title.isEmpty() ? item.id : item.title);
        const QString service = item.service;
        const QString path = item.path;
        connect(button, &IconButton::clicked, this,
            [this, service, path] { ctx().tray->activate(service, path, QStringLiteral("LeftClick")); });
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &IconButton::customContextMenuRequested, this,
            [this, service, path, button](const QPoint &pos) {
                ctx().tray->showContextMenu(service, path, button->mapToGlobal(pos));
            });
        layout->addWidget(button);
    }
}

} // namespace kapah
