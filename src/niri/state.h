// NiriState: the shell's mirror of the compositor.
//
// Roadmap Phase 1.5: "Maintain an in-memory model ... Expose it as a NiriState
// QObject with fine-grained signals (workspacesChanged, focusedWindowChanged,
// ...) so widgets repaint only what changed."
//
// The reducer is a pure function of the event stream: `apply(Event)` is the only
// mutator, and it is fully exercised by tests/fixtures/niri/*.jsonl. No widget
// ever talks to IpcClient directly.
//
// Fine-grained signals, and who listens:
//   outputsChanged            -> Bar (create/destroy per-output surfaces)
//   workspacesChanged         -> Bar workspaces module, launcher window switcher
//   workspaceActivatedChanged -> Bar workspaces module
//   workspaceUrgencyChanged   -> Bar workspaces module
//   windowsChanged            -> launcher window switcher
//   windowOpened/Closed       -> launcher window switcher, MPRIS-ish helpers
//   windowChanged(id)         -> bar window-title module
//   focusedWindowChanged      -> bar window-title module
//   windowUrgencyChanged(id)  -> bar workspaces module (workspace urgency)
//   keyboardLayoutsChanged    -> bar layout module
//   keyboardLayoutSwitched    -> bar layout module
//   overviewChanged           -> bar auto-hide
//   castsChanged              -> OSD "recording" indicator
//   reset()                   -> everything, after a reconnect

#pragma once

#include "protocol.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QVector>

namespace kapah::niri {

class IpcClient;

class NiriState : public QObject {
    Q_OBJECT
public:
    explicit NiriState(QObject *parent = nullptr);
    ~NiriState() override;

    // Wires up the client. NiriState owns no socket itself.
    void attach(IpcClient *client);

    // True once the event stream has delivered its first full snapshot. Until
    // then every getter returns an empty-but-valid value and `isReady()` is
    // false, which is what lets the bar draw "waiting for niri" instead of
    // guessing.
    bool isReady() const { return m_ready; }

    // --- outputs -----------------------------------------------------------
    const QVector<Output> &outputs() const { return m_outputs; }
    bool hasOutput(const QString &name) const;
    const Output *output(const QString &name) const;
    QString focusedOutputName() const { return m_focusedOutput; }

    // --- workspaces --------------------------------------------------------
    // Sorted by output, then by index. This is the order the bar renders in.
    const QVector<WorkspaceInfo> &workspaces() const { return m_workspaces; }
    QVector<WorkspaceInfo> workspacesForOutput(const QString &outputName) const;
    const WorkspaceInfo *workspace(quint64 id) const;
    const WorkspaceInfo *focusedWorkspace() const;
    // The workspace currently active on `outputName`, or nullptr.
    const WorkspaceInfo *activeWorkspaceOn(const QString &outputName) const;
    bool anyWorkspaceUrgent() const { return m_anyWorkspaceUrgent; }

    // --- windows -----------------------------------------------------------
    const QVector<WindowInfo> &windows() const { return m_windows; }
    const WindowInfo *window(quint64 id) const;
    const WindowInfo *focusedWindow() const;
    // Windows ordered by niri's focus timestamp, most recent first. This is what
    // the launcher's window switcher shows.
    QVector<quint64> windowsByRecency() const;
    int windowCount() const { return static_cast<int>(m_windows.size()); }

    // --- keyboard ----------------------------------------------------------
    const KeyboardLayouts &keyboardLayouts() const { return m_keyboards; }

    // --- misc --------------------------------------------------------------
    bool isOverviewOpen() const { return m_overviewOpen; }
    bool hasCasts() const { return !m_casts.isEmpty(); }
    bool anyCastActive() const { return m_anyCastActive; }

    // The screenshot niri reported, if any since the last call. Used by the OSD.
    QString takeLastScreenshotPath();

    // --- reducer -----------------------------------------------------------
    // Applies one event. Public so tests can drive it from a fixture.
    void apply(const Event &ev);

    // Drops all state and marks the model not-ready. Called on disconnect.
    void reset();

    // Number of unknown event variants seen; the integration tests assert on it.
    int unknownEventCount() const { return m_unknownEvents; }

Q_SIGNALS:
    void ready();
    void reset_();

    void outputsChanged();
    void focusedOutputChanged();

    void workspacesChanged();
    void workspaceActivatedChanged();
    void workspaceUrgencyChanged(quint64 id, bool urgent);

    void windowsChanged();
    void windowOpened(quint64 id);
    void windowClosed(quint64 id);
    void windowChanged(quint64 id);
    void windowUrgencyChanged(quint64 id, bool urgent);
    void focusedWindowChanged();

    void keyboardLayoutsChanged();
    void keyboardLayoutSwitched(int index);

    void overviewChanged(bool isOpen);

    void castsChanged();
    void screenshotCaptured(const QString &path);

    void unknownEvent(const QString &variant);

private:
    void applyWorkspacesChanged(const QVector<WorkspaceInfo> &workspaces);
    void applyWindowsChanged(const QVector<WindowInfo> &windows);
    void applyWindowOpenedOrChanged(const WindowInfo &w);
    void applyWindowClosed(quint64 id);
    void applyWindowFocusChanged(bool hasId, quint64 id);
    void applyWindowUrgency(quint64 id, bool urgent);
    void applyWorkspaceUrgency(quint64 id, bool urgent);
    void applyWorkspaceActivated(quint64 id, bool focused);
    void applyWorkspaceActiveWindowChanged(quint64 workspaceId, bool hasWindow, quint64 windowId);
    void applyKeyboardLayoutSwitched(int index);
    void recomputeDerived();

    IpcClient *m_client = nullptr;

    bool m_ready = false;
    int m_unknownEvents = 0;

    QVector<Output> m_outputs;
    QString m_focusedOutput;

    QVector<WorkspaceInfo> m_workspaces;
    QHash<quint64, int> m_workspaceIndex; // id -> position in m_workspaces
    bool m_anyWorkspaceUrgent = false;

    QVector<WindowInfo> m_windows;
    QHash<quint64, int> m_windowIndex; // id -> position in m_windows
    bool m_hasFocusedWindow = false;
    quint64 m_focusedWindowId = 0;

    KeyboardLayouts m_keyboards;

    bool m_overviewOpen = false;

    QVector<CastInfo> m_casts;
    bool m_anyCastActive = false;

    QString m_lastScreenshotPath;
};

} // namespace kapah::niri