// Typed wrapper over niri's Action request.
//
// Everything the shell does *to* niri goes through here, so there is exactly one
// place that knows the wire spelling of an action and exactly one place that
// decides whether to wait for a reply. Roadmap 4.4: no blocking D-Bus calls on
// the main thread -- same rule here for IPC: fire and forget for user actions,
// callbacks only where a result changes what we draw.

#pragma once

#include "protocol.h"

#include <QObject>
#include <QString>

namespace kapah::niri {

class IpcClient;

class ActionSender : public QObject {
    Q_OBJECT
public:
    explicit ActionSender(IpcClient *client, QObject *parent = nullptr);

    // Every method is fire-and-forget unless the name says otherwise. Errors are
    // logged by IpcClient; there is nothing useful for a bar button to do about
    // "niri said no".

    // Workspaces. `workspaceId` is the stable id from NiriState.
    void focusWorkspace(quint64 workspaceId);
    void focusWorkspaceIndex(int index);
    void focusWorkspaceDown();
    void focusWorkspaceUp();
    void focusWorkspacePrevious();
    void moveWorkspaceDown();
    void moveWorkspaceUp();
    void setWorkspaceName(quint64 workspaceId, const QString &name);
    void unsetWorkspaceName(quint64 workspaceId);

    // Windows.
    void focusWindow(quint64 windowId);
    void closeWindow(quint64 windowId);
    void closeFocusedWindow();
    void toggleFullscreen(quint64 windowId = 0);
    void toggleFloating(quint64 windowId = 0);

    // Outputs.
    void focusMonitor(const QString &outputName);
    void focusMonitorNext();
    void focusMonitorPrevious();
    void powerOffMonitors();
    void powerOnMonitors();

    // Overview.
    void toggleOverview();
    void openOverview();
    void closeOverview();

    // Keyboard.
    void switchLayoutNext();
    void switchLayoutPrev();
    void switchLayoutIndex(int index);

    // Session.
    void quit(bool skipConfirmation = true);

    // Screenshots: delegated to niri's own actions (Phase 9.4). The shell only
    // offers launch entries for these, but the power menu and keybinds use them.
    void screenshotInteractive();
    void screenshotScreen();
    void screenshotWindow();

    // Reports whether the last action succeeded. Only the few places that change
    // the UI based on the result use this.
    void withReply(const QJsonObject &actionJson, std::function<void(bool ok, const QString &err)> cb);

Q_SIGNALS:
    void actionFailed(const QString &message);

private:
    void send(const QJsonObject &actionJson);

    IpcClient *m_client = nullptr;
};

} // namespace kapah::niri