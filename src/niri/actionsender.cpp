#include "actionsender.h"

#include "ipcclient.h"
#include "logging.h"

namespace kapah::niri {

ActionSender::ActionSender(IpcClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

void ActionSender::send(const QJsonObject &actionJson)
{
    if (actionJson.isEmpty()) {
        LOG_WARN(Niri, "refusing to send an empty action");
        return;
    }
    if (m_client == nullptr) {
        LOG_WARN(Niri, "no IPC client; action dropped");
        return;
    }
    m_client->action(actionJson);
}

void ActionSender::withReply(
    const QJsonObject &actionJson, std::function<void(bool, const QString &)> cb)
{
    if (m_client == nullptr || actionJson.isEmpty()) {
        if (cb) {
            cb(false, QStringLiteral("niri is not running"));
        }
        return;
    }
    m_client->actionWithReply(actionJson, [this, cb](const Reply &r) {
        if (!r.ok) {
            Q_EMIT actionFailed(r.errorMessage);
        }
        if (cb) {
            cb(r.ok, r.errorMessage);
        }
    });
}

// ---------------------------------------------------------------------------
// Workspaces
// ---------------------------------------------------------------------------

void ActionSender::focusWorkspace(quint64 workspaceId)
{
    send(actionFocusWorkspace(workspaceId));
}

void ActionSender::focusWorkspaceIndex(int index)
{
    send(actionFocusWorkspaceIndex(index));
}

void ActionSender::focusWorkspaceDown()
{
    send(buildAction(ActionId::FocusWorkspaceDown));
}

void ActionSender::focusWorkspaceUp()
{
    send(buildAction(ActionId::FocusWorkspaceUp));
}

void ActionSender::focusWorkspacePrevious()
{
    send(buildAction(ActionId::FocusWorkspacePrevious));
}

void ActionSender::moveWorkspaceDown()
{
    send(buildAction(ActionId::MoveWorkspaceDown));
}

void ActionSender::moveWorkspaceUp()
{
    send(buildAction(ActionId::MoveWorkspaceUp));
}

void ActionSender::setWorkspaceName(quint64 workspaceId, const QString &name)
{
    QJsonObject extra;
    extra.insert(QStringLiteral("name"), name);
    // FocusWorkspace's `reference` is the documented way to name a workspace,
    // but niri has a dedicated SetWorkspaceName which takes a reference too.
    send(buildAction(ActionId::SetWorkspaceName, true, workspaceId, extra));
}

void ActionSender::unsetWorkspaceName(quint64 workspaceId)
{
    send(buildAction(ActionId::UnsetWorkspaceName, true, workspaceId));
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void ActionSender::focusWindow(quint64 windowId)
{
    send(actionFocusWindow(windowId));
}

void ActionSender::closeWindow(quint64 windowId)
{
    send(actionCloseWindow(windowId));
}

void ActionSender::closeFocusedWindow()
{
    // id = null tells niri "the focused one".
    send(actionCloseWindow(0));
}

void ActionSender::toggleFullscreen(quint64 windowId)
{
    if (windowId == 0) {
        send(buildAction(ActionId::ToggleWindowedFullscreen));
    } else {
        send(buildAction(ActionId::FullscreenWindow, true, windowId));
    }
}

void ActionSender::toggleFloating(quint64 windowId)
{
    if (windowId == 0) {
        send(buildAction(ActionId::ToggleWindowFloating));
    } else {
        send(buildAction(ActionId::ToggleWindowFloating, true, windowId));
    }
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------

void ActionSender::focusMonitor(const QString &outputName)
{
    send(actionFocusMonitor(outputName));
}

void ActionSender::focusMonitorNext()
{
    send(buildAction(ActionId::FocusMonitorNext));
}

void ActionSender::focusMonitorPrevious()
{
    send(buildAction(ActionId::FocusMonitorPrevious));
}

void ActionSender::powerOffMonitors()
{
    send(buildAction(ActionId::PowerOffMonitors));
}

void ActionSender::powerOnMonitors()
{
    send(buildAction(ActionId::PowerOnMonitors));
}

// ---------------------------------------------------------------------------
// Overview / keyboard / session
// ---------------------------------------------------------------------------

void ActionSender::toggleOverview()
{
    send(buildAction(ActionId::ToggleOverview));
}

void ActionSender::openOverview()
{
    send(buildAction(ActionId::OpenOverview));
}

void ActionSender::closeOverview()
{
    send(buildAction(ActionId::CloseOverview));
}

void ActionSender::switchLayoutNext()
{
    send(actionSwitchLayout(QStringLiteral("next")));
}

void ActionSender::switchLayoutPrev()
{
    send(actionSwitchLayout(QStringLiteral("prev")));
}

void ActionSender::switchLayoutIndex(int index)
{
    send(actionSwitchLayout(QString::number(index)));
}

void ActionSender::quit(bool skipConfirmation)
{
    send(actionQuit(skipConfirmation));
}

void ActionSender::screenshotInteractive()
{
    send(buildAction(ActionId::Screenshot));
}

void ActionSender::screenshotScreen()
{
    send(buildAction(ActionId::ScreenshotScreen));
}

void ActionSender::screenshotWindow()
{
    send(buildAction(ActionId::ScreenshotWindow));
}

} // namespace kapah::niri