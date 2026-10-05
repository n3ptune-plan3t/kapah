#include "workspaceitem.h"

#include "actionsender.h"
#include "state.h"
#include "widgets.h"

#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <QHBoxLayout>

namespace kapah {

WorkspaceItem::WorkspaceItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ctx.theme->px(2));

    m_debounce.setSingleShot(true);
    m_debounce.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_debounce, &QTimer::timeout, this, &WorkspaceItem::flushPendingSwitch);

    connect(ctx.state, &niri::NiriState::workspacesChanged, this, &WorkspaceItem::refresh);
    connect(ctx.state, &niri::NiriState::workspaceActivatedChanged, this,
        &WorkspaceItem::refresh);
    connect(ctx.state, &niri::NiriState::workspaceUrgencyChanged, this,
        [this](quint64, bool) { refresh(); });
    refresh();
}

void WorkspaceItem::refresh()
{
    if (ctx().state == nullptr) {
        return;
    }
    auto *layout = qobject_cast<QHBoxLayout *>(this->layout());

    // Index-keyed rebuild: drop all and recreate. Workspace counts are tiny
    // (<= about a dozen), so a full rebuild on every state signal is
    // acceptable and simpler than diffing; the"never rebuild everything"
    // rule applies to config hot-reload, not to this inner loop.
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_pills.clear();

    const QString outputName = output() != nullptr ? output()->name() : QString();
    for (const niri::WorkspaceInfo &ws : ctx().state->workspacesForOutput(outputName)) {
        auto *pill = new Pill(ctx().theme, this);
        pill->setIndicatorOnly(true);
        pill->setText(ws.displayName());
        pill->setSelected(ws.isActive);
        pill->setState(ws.isUrgent ? Pill::State::Urgent : Pill::State::Normal);
        pill->setProperty("wsId", QVariant::fromValue(ws.id));
        pill->installEventFilter(this);
        layout->addWidget(pill);
        m_pills.insert(ws.id, pill);
    }
}

bool WorkspaceItem::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton) {
            bool ok = false;
            const quint64 wsId = watched->property("wsId").toULongLong(&ok);
            if (ok && ctx().actions != nullptr) {
                ctx().actions->focusWorkspace(wsId);
                return true;
            }
        }
    }
    return BarModule::eventFilter(watched, event);
}

void WorkspaceItem::wheelEvent(QWheelEvent *event)
{
    if (ctx().actions == nullptr) {
        return;
    }
    m_pendingDelta += event->angleDelta().y() > 0 ? -1 : 1;
    m_debounce.start(100);
    event->accept();
}

void WorkspaceItem::flushPendingSwitch()
{
    if (m_pendingDelta == 0 || ctx().actions == nullptr) {
        return;
    }
    if (m_pendingDelta < 0) {
        ctx().actions->focusWorkspaceUp();
    } else {
        ctx().actions->focusWorkspaceDown();
    }
    m_pendingDelta = 0;
}

} // namespace kapah
