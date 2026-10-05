#include "state.h"

#include "ipcclient.h"
#include "logging.h"

#include <algorithm>

namespace kapah::niri {

NiriState::NiriState(QObject *parent)
    : QObject(parent)
{
}

NiriState::~NiriState() = default;

void NiriState::attach(IpcClient *client)
{
    m_client = client;
    if (m_client == nullptr) {
        return;
    }

    connect(m_client, &IpcClient::eventReceived, this, &NiriState::apply);

    connect(m_client, &IpcClient::eventStreamDisconnected, this, [this](const QString &why) {
        LOG_WARNF(Niri, "event stream lost (%s); state cleared until it returns", qPrintable(why));
        reset();
    });

    connect(m_client, &IpcClient::eventStreamConnected, this, [this] {
        // On reconnect niri re-sends the full state up front (WorkspacesChanged
        // and WindowsChanged carry complete snapshots), so nothing has to be
        // re-requested. We only need to remember that we are not ready until
        // those arrive.
        LOG_INFO(Niri, "event stream (re)connected; awaiting full state");
        reset();
    });
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

bool NiriState::hasOutput(const QString &name) const
{
    for (const Output &o : m_outputs) {
        if (o.name == name) {
            return true;
        }
    }
    return false;
}

const Output *NiriState::output(const QString &name) const
{
    for (const Output &o : m_outputs) {
        if (o.name == name) {
            return &o;
        }
    }
    return nullptr;
}

QVector<WorkspaceInfo> NiriState::workspacesForOutput(const QString &outputName) const
{
    QVector<WorkspaceInfo> out;
    for (const WorkspaceInfo &w : m_workspaces) {
        if (w.hasOutput && w.output == outputName) {
            out.append(w);
        }
    }
    std::sort(out.begin(), out.end(), [](const WorkspaceInfo &a, const WorkspaceInfo &b) {
        return a.idx < b.idx;
    });
    return out;
}

const WorkspaceInfo *NiriState::workspace(quint64 id) const
{
    const auto it = m_workspaceIndex.constFind(id);
    if (it == m_workspaceIndex.constEnd()) {
        return nullptr;
    }
    return &m_workspaces.at(it.value());
}

const WorkspaceInfo *NiriState::focusedWorkspace() const
{
    for (const WorkspaceInfo &w : m_workspaces) {
        if (w.isFocused) {
            return &w;
        }
    }
    return nullptr;
}

const WorkspaceInfo *NiriState::activeWorkspaceOn(const QString &outputName) const
{
    for (const WorkspaceInfo &w : m_workspaces) {
        if (w.isActive && w.hasOutput && w.output == outputName) {
            return &w;
        }
    }
    return nullptr;
}

const WindowInfo *NiriState::window(quint64 id) const
{
    const auto it = m_windowIndex.constFind(id);
    if (it == m_windowIndex.constEnd()) {
        return nullptr;
    }
    return &m_windows.at(it.value());
}

const WindowInfo *NiriState::focusedWindow() const
{
    if (!m_hasFocusedWindow) {
        return nullptr;
    }
    return window(m_focusedWindowId);
}

QVector<quint64> NiriState::windowsByRecency() const
{
    QVector<const WindowInfo *> sorted;
    sorted.reserve(m_windows.size());
    for (const WindowInfo &w : m_windows) {
        sorted.append(&w);
    }
    std::sort(sorted.begin(), sorted.end(), [](const WindowInfo *a, const WindowInfo *b) {
        // Windows without a focus timestamp (never focused) sort last.
        if (a->hasFocusTimestamp != b->hasFocusTimestamp) {
            return a->hasFocusTimestamp;
        }
        if (a->focusTimestampSecs != b->focusTimestampSecs) {
            return a->focusTimestampSecs > b->focusTimestampSecs;
        }
        if (a->focusTimestampNanos != b->focusTimestampNanos) {
            return a->focusTimestampNanos > b->focusTimestampNanos;
        }
        // Deterministic tiebreak so the launcher list does not jump around.
        return a->id > b->id;
    });

    QVector<quint64> ids;
    ids.reserve(sorted.size());
    for (const WindowInfo *w : sorted) {
        ids.append(w->id);
    }
    return ids;
}

QString NiriState::takeLastScreenshotPath()
{
    QString p;
    p.swap(m_lastScreenshotPath);
    return p;
}

void NiriState::reset()
{
    const bool wasReady = m_ready;
    m_ready = false;
    m_outputs.clear();
    m_focusedOutput.clear();
    m_workspaces.clear();
    m_workspaceIndex.clear();
    m_anyWorkspaceUrgent = false;
    m_windows.clear();
    m_windowIndex.clear();
    m_hasFocusedWindow = false;
    m_focusedWindowId = 0;
    m_keyboards = KeyboardLayouts {};
    m_overviewOpen = false;
    m_casts.clear();
    m_anyCastActive = false;
    m_lastScreenshotPath.clear();

    if (wasReady) {
        Q_EMIT reset_();
    }
    Q_EMIT outputsChanged();
    Q_EMIT windowsChanged();
    Q_EMIT workspacesChanged();
    Q_EMIT focusedWindowChanged();
}

// ---------------------------------------------------------------------------
// Reducer
// ---------------------------------------------------------------------------

void NiriState::apply(const Event &ev)
{
    switch (ev.kind) {
    case EventKind::WorkspacesChanged:
        applyWorkspacesChanged(ev.workspaces);
        break;
    case EventKind::WorkspaceUrgencyChanged:
        applyWorkspaceUrgency(ev.id, ev.urgent);
        break;
    case EventKind::WorkspaceActivated:
        applyWorkspaceActivated(ev.id, ev.focused);
        break;
    case EventKind::WorkspaceActiveWindowChanged:
        applyWorkspaceActiveWindowChanged(ev.workspaceId, ev.hasId, ev.id);
        break;
    case EventKind::WindowsChanged:
        applyWindowsChanged(ev.windows);
        break;
    case EventKind::WindowOpenedOrChanged:
        if (ev.window.id != 0) {
            applyWindowOpenedOrChanged(ev.window);
        }
        break;
    case EventKind::WindowClosed:
        applyWindowClosed(ev.id);
        break;
    case EventKind::WindowFocusChanged:
        applyWindowFocusChanged(ev.hasId, ev.id);
        break;
    case EventKind::WindowFocusTimestampChanged: {
        WindowInfo *w = nullptr;
        const auto it = m_windowIndex.constFind(ev.id);
        if (it != m_windowIndex.constEnd()) {
            w = &m_windows[it.value()];
        }
        if (w != nullptr) {
            w->hasFocusTimestamp = ev.hasTimestamp;
            w->focusTimestampSecs = ev.timestampSecs;
            w->focusTimestampNanos = ev.timestampNanos;
            // Nothing repaints on a timestamp change by itself, but the launcher's
            // window switcher sorts by it, so tell anyone who cares.
            Q_EMIT windowChanged(ev.id);
        }
        break;
    }
    case EventKind::WindowUrgencyChanged:
        applyWindowUrgency(ev.id, ev.urgent);
        break;
    case EventKind::WindowLayoutsChanged: {
        // Layout changes do not affect the bar or the launcher, but the window
        // switcher highlights the focused tile, so anything holding a WindowInfo
        // copy needs to know. One signal per window keeps repaints minimal.
        for (const auto &pair : ev.layoutChanges) {
            const auto it = m_windowIndex.constFind(pair.first);
            if (it == m_windowIndex.constEnd()) {
                continue;
            }
            m_windows[it.value()].layout = pair.second;
            Q_EMIT windowChanged(pair.first);
        }
        break;
    }
    case EventKind::KeyboardLayoutsChanged:
        m_keyboards = ev.keyboardLayouts;
        Q_EMIT keyboardLayoutsChanged();
        break;
    case EventKind::KeyboardLayoutSwitched:
        applyKeyboardLayoutSwitched(ev.index);
        break;
    case EventKind::OverviewOpenedOrClosed:
        if (m_overviewOpen != ev.isOpen) {
            m_overviewOpen = ev.isOpen;
            Q_EMIT overviewChanged(m_overviewOpen);
        }
        break;
    case EventKind::ConfigLoaded:
        // Nothing to do: niri re-sends its state when the config changes.
        break;
    case EventKind::ScreenshotCaptured:
        if (ev.hasPath) {
            m_lastScreenshotPath = ev.path;
            Q_EMIT screenshotCaptured(ev.path);
        }
        break;
    case EventKind::CastsChanged: {
        m_casts = ev.casts;
        m_anyCastActive = false;
        for (const CastInfo &c : m_casts) {
            if (c.isActive) {
                m_anyCastActive = true;
                break;
            }
        }
        Q_EMIT castsChanged();
        break;
    }
    case EventKind::CastStartedOrChanged: {
        if (ev.casts.isEmpty()) {
            break;
        }
        const CastInfo &c = ev.casts.first();
        bool replaced = false;
        for (CastInfo &existing : m_casts) {
            if (existing.streamId == c.streamId) {
                existing = c;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            m_casts.append(c);
        }
        m_anyCastActive = false;
        for (const CastInfo &cc : m_casts) {
            if (cc.isActive) {
                m_anyCastActive = true;
                break;
            }
        }
        Q_EMIT castsChanged();
        break;
    }
    case EventKind::CastStopped: {
        const qsizetype before = m_casts.size();
        m_casts.removeIf([ev](const CastInfo &c) { return c.streamId == ev.id; });
        if (m_casts.size() != before) {
            m_anyCastActive = false;
            for (const CastInfo &cc : m_casts) {
                if (cc.isActive) {
                    m_anyCastActive = true;
                    break;
                }
            }
            Q_EMIT castsChanged();
        }
        break;
    }
    case EventKind::Unknown:
        ++m_unknownEvents;
        Q_EMIT unknownEvent(ev.unknownKind);
        break;
    }

    if (!m_ready && (ev.kind == EventKind::WindowsChanged
            || ev.kind == EventKind::WorkspacesChanged)) {
        // niri always sends a full snapshot first, so both of these together mean
        // the model is populated. Mark ready on the second one so we do not
        // briefly report a ready state with no workspaces.
        if (ev.kind == EventKind::WorkspacesChanged) {
            m_ready = true;
            Q_EMIT ready();
        }
    }
}

void NiriState::applyWorkspacesChanged(const QVector<WorkspaceInfo> &workspaces)
{
    const QVector<WorkspaceInfo> old = m_workspaces;

    m_workspaces = workspaces;
    // Stable render order: output name, then index. niri's array order is not
    // guaranteed and the bar would otherwise reshuffle.
    std::sort(m_workspaces.begin(), m_workspaces.end(),
        [](const WorkspaceInfo &a, const WorkspaceInfo &b) {
            const QString ao = a.hasOutput ? a.output : QStringLiteral("\uffff");
            const QString bo = b.hasOutput ? b.output : QStringLiteral("\uffff");
            if (ao != bo) {
                return ao < bo;
            }
            return a.idx < b.idx;
        });

    m_workspaceIndex.clear();
    for (int i = 0; i < m_workspaces.size(); ++i) {
        m_workspaceIndex.insert(m_workspaces.at(i).id, i);
    }

    m_anyWorkspaceUrgent = false;
    for (const WorkspaceInfo &w : m_workspaces) {
        if (w.isUrgent) {
            m_anyWorkspaceUrgent = true;
            break;
        }
    }

    Q_EMIT workspacesChanged();
    Q_EMIT workspaceActivatedChanged();

    // Report per-workspace urgency deltas so a module can repaint one indicator.
    for (const WorkspaceInfo &w : m_workspaces) {
        const auto it = std::find_if(old.begin(), old.end(),
            [&w](const WorkspaceInfo &o) { return o.id == w.id; });
        const bool oldUrgent = it != old.end() && it->isUrgent;
        if (oldUrgent != w.isUrgent) {
            Q_EMIT workspaceUrgencyChanged(w.id, w.isUrgent);
        }
    }
    for (const WorkspaceInfo &w : old) {
        if (!m_workspaceIndex.contains(w.id) && w.isUrgent) {
            Q_EMIT workspaceUrgencyChanged(w.id, false);
        }
    }
}

void NiriState::applyWorkspaceUrgency(quint64 id, bool urgent)
{
    const auto it = m_workspaceIndex.constFind(id);
    if (it == m_workspaceIndex.constEnd()) {
        // niri documents that state updates are not always atomic; an urgency
        // change for a workspace we have not seen yet is expected and harmless.
        return;
    }
    if (m_workspaces[it.value()].isUrgent == urgent) {
        return;
    }
    m_workspaces[it.value()].isUrgent = urgent;
    recomputeDerived();
    Q_EMIT workspaceUrgencyChanged(id, urgent);
}

void NiriState::applyWorkspaceActivated(quint64 id, bool focused)
{
    const auto it = m_workspaceIndex.constFind(id);
    if (it == m_workspaceIndex.constEnd()) {
        return;
    }
    WorkspaceInfo &target = m_workspaces[it.value()];

    // Every output has exactly one active workspace. If we are told workspace A
    // became active, every other workspace on the same output becomes inactive.
    if (target.hasOutput) {
        for (WorkspaceInfo &w : m_workspaces) {
            if (w.id != target.id && w.isActive && w.hasOutput && w.output == target.output) {
                w.isActive = false;
            }
            if (focused && w.isFocused && w.id != target.id) {
                w.isFocused = false;
            }
        }
    }
    if (focused) {
        for (WorkspaceInfo &w : m_workspaces) {
            if (w.id != target.id) {
                w.isFocused = false;
            }
        }
    }

    target.isActive = true;
    if (focused) {
        target.isFocused = true;
        m_focusedOutput = target.hasOutput ? target.output : m_focusedOutput;
    }

    Q_EMIT workspaceActivatedChanged();
    Q_EMIT workspacesChanged();

    // The focused output may have moved with the focus.
    const auto focusedIt = std::find_if(m_workspaces.cbegin(), m_workspaces.cend(),
        [](const WorkspaceInfo &w) { return w.isFocused; });
    const QString newFocusedOutput =
        focusedIt != m_workspaces.cend() && focusedIt->hasOutput ? focusedIt->output : QString();
    if (newFocusedOutput != m_focusedOutput) {
        m_focusedOutput = newFocusedOutput;
        Q_EMIT focusedOutputChanged();
        Q_EMIT outputsChanged();
    }
}

void NiriState::applyWorkspaceActiveWindowChanged(
    quint64 workspaceId, bool hasWindow, quint64 windowId)
{
    const auto it = m_workspaceIndex.constFind(workspaceId);
    if (it == m_workspaceIndex.constEnd()) {
        return;
    }
    WorkspaceInfo &w = m_workspaces[it.value()];
    w.hasActiveWindow = hasWindow;
    w.activeWindowId = hasWindow ? windowId : 0;

    // The launcher window switcher highlights the active window per workspace.
    Q_EMIT workspacesChanged();
}

void NiriState::applyWindowsChanged(const QVector<WindowInfo> &windows)
{
    const QSet<quint64> oldIds = [this] {
        QSet<quint64> ids;
        for (const WindowInfo &w : m_windows) {
            ids.insert(w.id);
        }
        return ids;
    }();

    m_windows = windows;
    m_windowIndex.clear();
    for (int i = 0; i < m_windows.size(); ++i) {
        m_windowIndex.insert(m_windows.at(i).id, i);
    }

    // is_focused is authoritative in a full snapshot.
    m_hasFocusedWindow = false;
    m_focusedWindowId = 0;
    for (const WindowInfo &w : m_windows) {
        if (w.isFocused) {
            m_hasFocusedWindow = true;
            m_focusedWindowId = w.id;
            break;
        }
    }

    Q_EMIT windowsChanged();
    Q_EMIT focusedWindowChanged();

    for (const WindowInfo &w : m_windows) {
        if (!oldIds.contains(w.id)) {
            Q_EMIT windowOpened(w.id);
        }
    }
    for (quint64 id : oldIds) {
        if (!m_windowIndex.contains(id)) {
            Q_EMIT windowClosed(id);
        }
    }

    // Windows may have moved workspaces without a separate event.
    Q_EMIT workspacesChanged();
}

void NiriState::applyWindowOpenedOrChanged(const WindowInfo &w)
{
    const auto it = m_windowIndex.constFind(w.id);
    const bool isNew = it == m_windowIndex.constEnd();

    if (isNew) {
        m_windows.append(w);
        m_windowIndex.insert(w.id, static_cast<int>(m_windows.size()) - 1);
    } else {
        m_windows[it.value()] = w;
    }

    // niri guarantees at most one focused window.
    if (w.isFocused) {
        for (WindowInfo &other : m_windows) {
            other.isFocused = (other.id == w.id);
        }
        if (!m_hasFocusedWindow || m_focusedWindowId != w.id) {
            m_hasFocusedWindow = true;
            m_focusedWindowId = w.id;
            Q_EMIT focusedWindowChanged();
        }
    } else if (m_hasFocusedWindow && m_focusedWindowId == w.id) {
        // A change event that drops is_focused means focus moved elsewhere; the
        // separate WindowFocusChanged event will tell us where.
        m_hasFocusedWindow = false;
        m_focusedWindowId = 0;
        Q_EMIT focusedWindowChanged();
    }

    if (isNew) {
        Q_EMIT windowOpened(w.id);
    } else {
        Q_EMIT windowChanged(w.id);
    }
    Q_EMIT windowsChanged();
}

void NiriState::applyWindowClosed(quint64 id)
{
    const auto it = m_windowIndex.constFind(id);
    if (it == m_windowIndex.constEnd()) {
        return;
    }
    m_windows.removeAt(it.value());
    // Removing shifts every later index.
    m_windowIndex.clear();
    for (int i = 0; i < m_windows.size(); ++i) {
        m_windowIndex.insert(m_windows.at(i).id, i);
    }

    if (m_hasFocusedWindow && m_focusedWindowId == id) {
        m_hasFocusedWindow = false;
        m_focusedWindowId = 0;
        Q_EMIT focusedWindowChanged();
    }

    Q_EMIT windowClosed(id);
    Q_EMIT windowsChanged();
}

void NiriState::applyWindowFocusChanged(bool hasId, quint64 id)
{
    const quint64 newId = hasId ? id : 0;
    if (m_hasFocusedWindow == hasId && m_focusedWindowId == newId) {
        return;
    }

    for (WindowInfo &w : m_windows) {
        w.isFocused = (w.id == newId);
    }
    m_hasFocusedWindow = hasId;
    m_focusedWindowId = newId;

    // Focus implies the focused window's workspace, which the bar's workspace
    // indicators depend on.
    const WindowInfo *fw = focusedWindow();
    if (fw != nullptr) {
        for (WorkspaceInfo &ws : m_workspaces) {
            ws.isFocused = fw->hasWorkspace && ws.id == fw->workspaceId;
        }
        if (fw->hasWorkspace) {
            m_focusedOutput.clear();
            const auto it = m_workspaceIndex.constFind(fw->workspaceId);
            if (it != m_workspaceIndex.constEnd() && m_workspaces[it.value()].hasOutput) {
                m_focusedOutput = m_workspaces[it.value()].output;
            }
        }
    }

    Q_EMIT focusedWindowChanged();
    Q_EMIT windowChanged(newId);
    Q_EMIT workspaceActivatedChanged();
    Q_EMIT focusedOutputChanged();
}

void NiriState::applyWindowUrgency(quint64 id, bool urgent)
{
    const auto it = m_windowIndex.constFind(id);
    if (it == m_windowIndex.constEnd()) {
        return;
    }
    if (m_windows[it.value()].isUrgent == urgent) {
        return;
    }
    m_windows[it.value()].isUrgent = urgent;

    // A window's urgency propagates to its workspace. niri sends a separate
    // WorkspaceUrgencyChanged, but recomputing here means the bar never shows a
    // stale urgent dot even if that event is reordered or dropped.
    const quint64 workspaceId = m_windows[it.value()].workspaceId;
    if (m_windows[it.value()].hasWorkspace) {
        const auto wsIt = m_workspaceIndex.constFind(workspaceId);
        if (wsIt != m_workspaceIndex.constEnd()) {
            bool workspaceUrgent = false;
            for (const WindowInfo &candidate : m_windows) {
                if (candidate.hasWorkspace && candidate.workspaceId == workspaceId
                    && candidate.isUrgent) {
                    workspaceUrgent = true;
                    break;
                }
            }
            if (m_workspaces[wsIt.value()].isUrgent != workspaceUrgent) {
                m_workspaces[wsIt.value()].isUrgent = workspaceUrgent;
            }
        }
    }

    recomputeDerived();
    Q_EMIT windowUrgencyChanged(id, urgent);
    Q_EMIT windowChanged(id);
}

void NiriState::applyKeyboardLayoutSwitched(int index)
{
    if (m_keyboards.currentIndex == index) {
        return;
    }
    m_keyboards.currentIndex = index;
    Q_EMIT keyboardLayoutSwitched(index);
    Q_EMIT keyboardLayoutsChanged();
}

void NiriState::recomputeDerived()
{
    bool urgent = false;
    for (const WorkspaceInfo &w : m_workspaces) {
        if (w.isUrgent) {
            urgent = true;
            break;
        }
    }
    if (urgent == m_anyWorkspaceUrgent) {
        return;
    }
    m_anyWorkspaceUrgent = urgent;
    Q_EMIT workspacesChanged();
}

} // namespace kapah::niri