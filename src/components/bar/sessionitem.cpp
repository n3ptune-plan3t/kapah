#include "sessionitem.h"

#include "actionsender.h"
#include "logind.h"
#include "popup.h"
#include "theme.h"
#include "widgets.h"

#include <QPushButton>

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace kapah {

SessionItem::SessionItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_button = new IconButton(ctx.theme, ctx.icons, this);
    m_button->setShape(IconButton::Shape::Circle);
    m_button->setIconName(QStringLiteral("system-shutdown-symbolic"));
    connect(m_button, &IconButton::clicked, this, &SessionItem::openMenu);
    layout->addWidget(m_button);
}

void SessionItem::openMenu()
{
    if (m_popup == nullptr) {
        m_popup = new Popup(ctx().theme, ctx().icons, QStringLiteral("kapah-session"), this);
        connect(m_popup, &Popup::dismissRequested, m_popup, &Popup::dismiss);
        auto *outer = new QVBoxLayout(m_popup);
        outer->setContentsMargins(0, 0, 0, 0);

        auto addButton = [this, outer](const QString &text, auto action) {
            auto *button = new QPushButton(text, m_popup);
            connect(button, &QPushButton::clicked, m_popup,
                [this, action] {
                    m_popup->dismiss();
                    action();
                });
            outer->addWidget(button);
        };

        addButton(QStringLiteral("Lock"), [this] {
            if (ctx().logind != nullptr) {
                ctx().logind->lockSession();
            }
        });
        addButton(QStringLiteral("Log out"), [this] {
            if (ctx().actions != nullptr) {
                ctx().actions->quit(true);
            }
        });
        addButton(QStringLiteral("Suspend"), [this] {
            if (ctx().logind != nullptr) {
                ctx().logind->suspend();
            }
        });
        if (ctx().logind != nullptr && ctx().logind->canHibernate()) {
            addButton(QStringLiteral("Hibernate"), [this] { ctx().logind->hibernate(); });
        }
        addButton(QStringLiteral("Reboot"), [this] {
            if (ctx().logind != nullptr) {
                ctx().logind->reboot();
            }
        });
        addButton(QStringLiteral("Shut down"), [this] {
            if (ctx().logind != nullptr) {
                ctx().logind->powerOff();
            }
        });
    }
    m_popup->setPlacement(Popup::Placement::BelowAnchor);
    m_popup->setAnchorWidget(m_button);
    m_popup->resizeToContent();
    m_popup->popup(output());
}

} // namespace kapah
