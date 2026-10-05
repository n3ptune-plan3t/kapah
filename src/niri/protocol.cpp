#include "protocol.h"

#include "logging.h"
#include "kapah/version.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace kapah::niri {

namespace {

struct KindName {
    RequestKind kind;
    const char *name;
};

constexpr KindName kRequestNames[] = {
    { RequestKind::Version, "Version" },
    { RequestKind::Outputs, "Outputs" },
    { RequestKind::Workspaces, "Workspaces" },
    { RequestKind::Windows, "Windows" },
    { RequestKind::Layers, "Layers" },
    { RequestKind::KeyboardLayouts, "KeyboardLayouts" },
    { RequestKind::FocusedOutput, "FocusedOutput" },
    { RequestKind::FocusedWindow, "FocusedWindow" },
    { RequestKind::PickWindow, "PickWindow" },
    { RequestKind::PickColor, "PickColor" },
    { RequestKind::Action, "Action" },
    { RequestKind::Output, "Output" },
    { RequestKind::EventStream, "EventStream" },
    { RequestKind::ReturnError, "ReturnError" },
    { RequestKind::OverviewState, "OverviewState" },
    { RequestKind::Casts, "Casts" },
};

} // namespace

QString requestKindToString(RequestKind kind)
{
    for (const KindName &k : kRequestNames) {
        if (k.kind == kind) {
            return QString::fromLatin1(k.name);
        }
    }
    return QStringLiteral("Unknown");
}

QByteArray encodeRequest(RequestKind kind, const QJsonObject &payload)
{
    const QString name = requestKindToString(kind);
    if (name == QLatin1String("Unknown")) {
        return QByteArray();
    }
    QJsonObject root;
    if (payload.isEmpty()) {
        // Serde serialises a unit variant as a bare string.
        return QJsonDocument(QJsonValue(name).toVariant()).toJson(QJsonDocument::Compact)
            + '\n';
    }
    // A newtype variant becomes {"Name": <payload>}.
    root.insert(name, payload);
    return QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
}

// ---------------------------------------------------------------------------
// Reply
// ---------------------------------------------------------------------------

Reply Reply::fromJson(const QJsonObject &obj)
{
    Reply reply;

    // Reply = Result<Response, String>, so exactly one of "Ok" / "Err".
    if (obj.contains(QLatin1String("Err"))) {
        reply.ok = false;
        reply.errorMessage = obj.value(QLatin1String("Err")).toString();
        reply.kind = ResponseKind::Error;
        return reply;
    }
    if (!obj.contains(QLatin1String("Ok"))) {
        reply.ok = false;
        reply.errorMessage = QStringLiteral("reply had neither Ok nor Err");
        reply.kind = ResponseKind::Error;
        return reply;
    }

    const QJsonValue okValue = obj.value(QLatin1String("Ok"));
    reply.ok = true;

    // A unit variant round-trips as a bare string.
    if (okValue.isString()) {
        const QString name = okValue.toString();
        if (name == QLatin1String("Handled")) {
            reply.kind = ResponseKind::Handled;
            return reply;
        }
        reply.kind = ResponseKind::Error;
        reply.errorMessage = QStringLiteral("unknown Response unit variant '%1'").arg(name);
        reply.ok = false;
        return reply;
    }

    if (!okValue.isObject()) {
        reply.ok = false;
        reply.kind = ResponseKind::Error;
        reply.errorMessage = QStringLiteral("Ok payload was neither object nor string");
        return reply;
    }

    const QJsonObject inner = okValue.toObject();
    // Exactly one key, the variant name.
    const auto keys = inner.keys();
    if (keys.isEmpty()) {
        reply.ok = false;
        reply.kind = ResponseKind::Error;
        reply.errorMessage = QStringLiteral("empty Ok object");
        return reply;
    }
    const QString variant = keys.first();
    reply.payload.insert(variant, inner.value(variant));

    static const QHash<QString, ResponseKind> map = {
        { QStringLiteral("Handled"), ResponseKind::Handled },
        { QStringLiteral("Version"), ResponseKind::Version },
        { QStringLiteral("Outputs"), ResponseKind::Outputs },
        { QStringLiteral("Workspaces"), ResponseKind::Workspaces },
        { QStringLiteral("Windows"), ResponseKind::Windows },
        { QStringLiteral("Layers"), ResponseKind::Layers },
        { QStringLiteral("KeyboardLayouts"), ResponseKind::KeyboardLayouts },
        { QStringLiteral("FocusedOutput"), ResponseKind::FocusedOutput },
        { QStringLiteral("FocusedWindow"), ResponseKind::FocusedWindow },
        { QStringLiteral("PickedWindow"), ResponseKind::PickedWindow },
        { QStringLiteral("PickedColor"), ResponseKind::PickedColor },
        { QStringLiteral("OutputConfigChanged"), ResponseKind::OutputConfigChanged },
        { QStringLiteral("OverviewState"), ResponseKind::OverviewState },
        { QStringLiteral("Casts"), ResponseKind::Casts },
    };
    const auto it = map.constFind(variant);
    if (it == map.constEnd()) {
        // A newer niri added a response variant. We do not need it; treat it as
        // handled-but-unknown rather than an error so a future niri cannot break
        // the shell.
        reply.kind = ResponseKind::Handled;
        LOG_CATF(Niri, "niri sent an unknown Response variant '%s'", qPrintable(variant));
        return reply;
    }
    reply.kind = it.value();
    return reply;
}

QString Reply::describe() const
{
    if (!ok) {
        return QStringLiteral("Err(%1)").arg(errorMessage);
    }
    return QStringLiteral("Ok(%1)").arg(QString::fromLatin1(payload.keys().value(0, "?")));
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

std::optional<Output> Output::fromJson(const QJsonObject &obj)
{
    Output out;
    out.name = obj.value(QStringLiteral("name")).toString();
    if (out.name.isEmpty()) {
        return std::nullopt;
    }
    out.make = obj.value(QStringLiteral("make")).toString();
    out.model = obj.value(QStringLiteral("model")).toString();
    if (obj.value(QStringLiteral("serial")).isString()) {
        out.serial = obj.value(QStringLiteral("serial")).toString();
    }

    if (obj.value(QStringLiteral("physical_size")).isArray()) {
        const auto arr = obj.value(QStringLiteral("physical_size")).toArray();
        if (arr.size() == 2) {
            out.hasPhysicalSize = true;
            out.physicalWidthMm = arr.at(0).toInt();
            out.physicalHeightMm = arr.at(1).toInt();
        }
    }

    if (obj.value(QStringLiteral("modes")).isArray()) {
        for (const QJsonValue &v : obj.value(QStringLiteral("modes")).toArray()) {
            if (v.isObject()) {
                out.modes.append(v.toObject());
            }
        }
    }

    if (obj.value(QStringLiteral("current_mode")).isDouble()) {
        out.currentModeIndex = static_cast<int>(obj.value(QStringLiteral("current_mode")).toDouble());
    }
    out.isCustomMode = obj.value(QStringLiteral("is_custom_mode")).toBool();
    out.vrrSupported = obj.value(QStringLiteral("vrr_supported")).toBool();
    out.vrrEnabled = obj.value(QStringLiteral("vrr_enabled")).toBool();

    const QJsonValue logical = obj.value(QStringLiteral("logical"));
    if (logical.isObject()) {
        const QJsonObject l = logical.toObject();
        out.isDisabled = false;
        out.x = l.value(QStringLiteral("x")).toInt();
        out.y = l.value(QStringLiteral("y")).toInt();
        out.width = l.value(QStringLiteral("width")).toInt();
        out.height = l.value(QStringLiteral("height")).toInt();
        out.scale = l.value(QStringLiteral("scale")).toDouble(1.0);
        // Transform is an externally-tagged unit enum: "Normal" | "90" | ...
        out.transform = l.value(QStringLiteral("transform")).toString();
    } else {
        out.isDisabled = true;
    }

    return out;
}

int Output::modeWidth() const
{
    if (currentModeIndex < 0 || currentModeIndex >= modes.size()) {
        return width;
    }
    return modes.at(currentModeIndex).value(QStringLiteral("width")).toInt(width);
}

int Output::modeHeight() const
{
    if (currentModeIndex < 0 || currentModeIndex >= modes.size()) {
        return height;
    }
    return modes.at(currentModeIndex).value(QStringLiteral("height")).toInt(height);
}

double Output::refreshHz() const
{
    if (currentModeIndex < 0 || currentModeIndex >= modes.size()) {
        return 0.0;
    }
    // niri reports millihertz.
    const double mhz = modes.at(currentModeIndex).value(QStringLiteral("refresh_rate")).toDouble();
    return mhz / 1000.0;
}

// ---------------------------------------------------------------------------
// WindowLayout / WindowInfo
// ---------------------------------------------------------------------------

WindowLayout WindowLayout::fromJson(const QJsonObject &obj)
{
    WindowLayout l;
    if (obj.value(QStringLiteral("pos_in_scrolling_layout")).isArray()) {
        const auto arr = obj.value(QStringLiteral("pos_in_scrolling_layout")).toArray();
        if (arr.size() == 2) {
            l.hasPosInScrollingLayout = true;
            l.column = arr.at(0).toInt();
            l.tile = arr.at(1).toInt();
        }
    }
    if (obj.value(QStringLiteral("tile_size")).isArray()) {
        const auto arr = obj.value(QStringLiteral("tile_size")).toArray();
        if (arr.size() == 2) {
            l.tileWidth = arr.at(0).toDouble();
            l.tileHeight = arr.at(1).toDouble();
        }
    }
    if (obj.value(QStringLiteral("window_size")).isArray()) {
        const auto arr = obj.value(QStringLiteral("window_size")).toArray();
        if (arr.size() == 2) {
            l.windowWidth = arr.at(0).toInt();
            l.windowHeight = arr.at(1).toInt();
        }
    }
    if (obj.value(QStringLiteral("tile_pos_in_workspace_view")).isArray()) {
        const auto arr = obj.value(QStringLiteral("tile_pos_in_workspace_view")).toArray();
        if (arr.size() == 2) {
            l.hasTilePosInView = true;
            l.viewX = arr.at(0).toDouble();
            l.viewY = arr.at(1).toDouble();
        }
    }
    if (obj.value(QStringLiteral("window_offset_in_tile")).isArray()) {
        const auto arr = obj.value(QStringLiteral("window_offset_in_tile")).toArray();
        if (arr.size() == 2) {
            l.offsetX = arr.at(0).toDouble();
            l.offsetY = arr.at(1).toDouble();
        }
    }
    return l;
}

WindowInfo WindowInfo::fromJson(const QJsonObject &obj)
{
    WindowInfo w;
    w.id = static_cast<quint64>(obj.value(QStringLiteral("id")).toDouble());
    if (obj.value(QStringLiteral("title")).isString()) {
        w.title = obj.value(QStringLiteral("title")).toString();
    }
    if (obj.value(QStringLiteral("app_id")).isString()) {
        w.appId = obj.value(QStringLiteral("app_id")).toString();
    }
    if (obj.value(QStringLiteral("pid")).isDouble()) {
        w.hasPid = true;
        w.pid = static_cast<qint32>(obj.value(QStringLiteral("pid")).toDouble());
    }
    if (obj.value(QStringLiteral("workspace_id")).isDouble()) {
        w.hasWorkspace = true;
        w.workspaceId = static_cast<quint64>(obj.value(QStringLiteral("workspace_id")).toDouble());
    }
    w.isFocused = obj.value(QStringLiteral("is_focused")).toBool();
    w.isFloating = obj.value(QStringLiteral("is_floating")).toBool();
    w.isUrgent = obj.value(QStringLiteral("is_urgent")).toBool();
    if (obj.value(QStringLiteral("layout")).isObject()) {
        w.layout = WindowLayout::fromJson(obj.value(QStringLiteral("layout")).toObject());
    }
    if (obj.value(QStringLiteral("focus_timestamp")).isObject()) {
        const QJsonObject ts = obj.value(QStringLiteral("focus_timestamp")).toObject();
        w.hasFocusTimestamp = true;
        w.focusTimestampSecs =
            static_cast<quint64>(ts.value(QStringLiteral("secs")).toDouble());
        w.focusTimestampNanos =
            static_cast<quint32>(ts.value(QStringLiteral("nanos")).toDouble());
    }
    return w;
}

// ---------------------------------------------------------------------------
// WorkspaceInfo
// ---------------------------------------------------------------------------

WorkspaceInfo WorkspaceInfo::fromJson(const QJsonObject &obj)
{
    WorkspaceInfo w;
    w.id = static_cast<quint64>(obj.value(QStringLiteral("id")).toDouble());
    w.idx = obj.value(QStringLiteral("idx")).toInt();
    if (obj.value(QStringLiteral("name")).isString()) {
        w.hasName = true;
        w.name = obj.value(QStringLiteral("name")).toString();
    }
    if (obj.value(QStringLiteral("output")).isString()) {
        w.hasOutput = true;
        w.output = obj.value(QStringLiteral("output")).toString();
    }
    w.isUrgent = obj.value(QStringLiteral("is_urgent")).toBool();
    w.isActive = obj.value(QStringLiteral("is_active")).toBool();
    w.isFocused = obj.value(QStringLiteral("is_focused")).toBool();
    if (obj.value(QStringLiteral("active_window_id")).isDouble()) {
        w.hasActiveWindow = true;
        w.activeWindowId =
            static_cast<quint64>(obj.value(QStringLiteral("active_window_id")).toDouble());
    }
    return w;
}

QString WorkspaceInfo::displayName() const
{
    if (hasName && !name.isEmpty()) {
        return name;
    }
    return QString::number(idx + 1);
}

// ---------------------------------------------------------------------------
// KeyboardLayouts
// ---------------------------------------------------------------------------

KeyboardLayouts KeyboardLayouts::fromJson(const QJsonObject &obj)
{
    KeyboardLayouts k;
    if (obj.value(QStringLiteral("names")).isArray()) {
        for (const QJsonValue &v : obj.value(QStringLiteral("names")).toArray()) {
            k.names.append(v.toString());
        }
    }
    k.currentIndex = obj.value(QStringLiteral("current_idx")).toInt();
    return k;
}

QString KeyboardLayouts::currentDisplayName() const
{
    if (currentIndex < 0 || currentIndex >= names.size()) {
        return {};
    }
    // niri reports the xkbcommon layout name ("us", "de", "gb"). Showing "us" in
    // a bar module is not useful, so map the common ones and fall back to upper
    // case otherwise.
    const QString xkb = names.at(currentIndex);
    static const QHash<QString, QString> friendly = {
        { QStringLiteral("us"), QStringLiteral("US") },
        { QStringLiteral("gb"), QStringLiteral("UK") },
        { QStringLiteral("de"), QStringLiteral("DE") },
        { QStringLiteral("fr"), QStringLiteral("FR") },
        { QStringLiteral("es"), QStringLiteral("ES") },
        { QStringLiteral("it"), QStringLiteral("IT") },
        { QStringLiteral("ru"), QStringLiteral("RU") },
        { QStringLiteral("se"), QStringLiteral("SE") },
        { QStringLiteral("no"), QStringLiteral("NO") },
        { QStringLiteral("dk"), QStringLiteral("DK") },
        { QStringLiteral("fi"), QStringLiteral("FI") },
        { QStringLiteral("pl"), QStringLiteral("PL") },
        { QStringLiteral("cz"), QStringLiteral("CZ") },
        { QStringLiteral("ua"), QStringLiteral("UA") },
        { QStringLiteral("jp"), QStringLiteral("JP") },
        { QStringLiteral("kr"), QStringLiteral("KR") },
        { QStringLiteral("cn"), QStringLiteral("CN") },
        { QStringLiteral("latam"), QStringLiteral("LATAM") },
        { QStringLiteral("ara"), QStringLiteral("ARA") },
    };
    const auto it = friendly.constFind(xkb);
    if (it != friendly.constEnd()) {
        return it.value();
    }
    return xkb.toUpper();
}

// ---------------------------------------------------------------------------
// LayerSurfaceInfo / CastInfo
// ---------------------------------------------------------------------------

LayerSurfaceInfo LayerSurfaceInfo::fromJson(const QJsonObject &obj)
{
    LayerSurfaceInfo l;
    l.nameSpace = obj.value(QStringLiteral("namespace")).toString();
    l.output = obj.value(QStringLiteral("output")).toString();
    l.layer = obj.value(QStringLiteral("layer")).toString();
    l.keyboardInteractivity =
        obj.value(QStringLiteral("keyboard_interactivity")).toString();
    return l;
}

CastInfo CastInfo::fromJson(const QJsonObject &obj)
{
    CastInfo c;
    c.streamId = static_cast<quint64>(obj.value(QStringLiteral("stream_id")).toDouble());
    c.sessionId = static_cast<quint64>(obj.value(QStringLiteral("session_id")).toDouble());
    c.kind = obj.value(QStringLiteral("kind")).toString();
    c.isDynamicTarget = obj.value(QStringLiteral("is_dynamic_target")).toBool();
    c.isActive = obj.value(QStringLiteral("is_active")).toBool();
    if (obj.value(QStringLiteral("pid")).isDouble()) {
        c.hasPid = true;
        c.pid = static_cast<qint32>(obj.value(QStringLiteral("pid")).toDouble());
    }
    if (obj.value(QStringLiteral("pw_node_id")).isDouble()) {
        c.hasPipeWireNode = true;
        c.pipeWireNodeId =
            static_cast<quint32>(obj.value(QStringLiteral("pw_node_id")).toDouble());
    }
    if (obj.value(QStringLiteral("target")).isObject()) {
        const QJsonObject t = obj.value(QStringLiteral("target")).toObject();
        if (t.contains(QStringLiteral("name"))) {
            c.targetDescription = QStringLiteral("output %1").arg(t.value(QStringLiteral("name")).toString());
        } else if (t.contains(QStringLiteral("id"))) {
            c.targetDescription =
                QStringLiteral("window %1").arg(t.value(QStringLiteral("id")).toInt());
        } else {
            c.targetDescription = QStringLiteral("nothing");
        }
    }
    return c;
}

OutputConfigChanged OutputConfigChanged::fromJson(const QJsonObject &obj)
{
    OutputConfigChanged c;
    c.result = obj.value(QStringLiteral("result")).toString();
    return c;
}

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

namespace {

struct EventName {
    EventKind kind;
    const char *name;
};

constexpr EventName kEventNames[] = {
    { EventKind::WorkspacesChanged, "WorkspacesChanged" },
    { EventKind::WorkspaceUrgencyChanged, "WorkspaceUrgencyChanged" },
    { EventKind::WorkspaceActivated, "WorkspaceActivated" },
    { EventKind::WorkspaceActiveWindowChanged, "WorkspaceActiveWindowChanged" },
    { EventKind::WindowsChanged, "WindowsChanged" },
    { EventKind::WindowOpenedOrChanged, "WindowOpenedOrChanged" },
    { EventKind::WindowClosed, "WindowClosed" },
    { EventKind::WindowFocusChanged, "WindowFocusChanged" },
    { EventKind::WindowFocusTimestampChanged, "WindowFocusTimestampChanged" },
    { EventKind::WindowUrgencyChanged, "WindowUrgencyChanged" },
    { EventKind::WindowLayoutsChanged, "WindowLayoutsChanged" },
    { EventKind::KeyboardLayoutsChanged, "KeyboardLayoutsChanged" },
    { EventKind::KeyboardLayoutSwitched, "KeyboardLayoutSwitched" },
    { EventKind::OverviewOpenedOrClosed, "OverviewOpenedOrClosed" },
    { EventKind::ConfigLoaded, "ConfigLoaded" },
    { EventKind::ScreenshotCaptured, "ScreenshotCaptured" },
    { EventKind::CastsChanged, "CastsChanged" },
    { EventKind::CastStartedOrChanged, "CastStartedOrChanged" },
    { EventKind::CastStopped, "CastStopped" },
};

} // namespace

QString Event::kindToString() const
{
    for (const EventName &e : kEventNames) {
        if (e.kind == kind) {
            return QString::fromLatin1(e.name);
        }
    }
    return unknownKind.isEmpty() ? QStringLiteral("Unknown") : unknownKind;
}

Event Event::fromJson(const QJsonObject &obj, bool *ok)
{
    Event ev;
    if (ok != nullptr) {
        *ok = true;
    }

    const QStringList keys = obj.keys();
    if (keys.isEmpty()) {
        if (ok != nullptr) {
            *ok = false;
        }
        return ev;
    }
    const QString variant = keys.first();
    const QJsonValue payload = obj.value(variant);

    // Unknown variant: not an error. A newer niri adds variants; we log once and
    // ignore. This is the version-tolerance policy from docs/PROTOCOLS.md.
    EventKind kind = EventKind::Unknown;
    for (const EventName &e : kEventNames) {
        if (variant == QLatin1String(e.name)) {
            kind = e.kind;
            break;
        }
    }
    if (kind == EventKind::Unknown) {
        ev.kind = EventKind::Unknown;
        ev.unknownKind = variant;
        return ev;
    }
    ev.kind = kind;

    const QJsonObject p = payload.isObject() ? payload.toObject() : QJsonObject();

    switch (kind) {
    case EventKind::WorkspacesChanged:
        if (p.value(QStringLiteral("workspaces")).isArray()) {
            for (const QJsonValue &v : p.value(QStringLiteral("workspaces")).toArray()) {
                if (v.isObject()) {
                    ev.workspaces.append(WorkspaceInfo::fromJson(v.toObject()));
                }
            }
        }
        break;

    case EventKind::WorkspaceUrgencyChanged:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        ev.urgent = p.value(QStringLiteral("urgent")).toBool();
        break;

    case EventKind::WorkspaceActivated:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        ev.focused = p.value(QStringLiteral("focused")).toBool();
        break;

    case EventKind::WorkspaceActiveWindowChanged:
        ev.workspaceId =
            static_cast<quint64>(p.value(QStringLiteral("workspace_id")).toDouble());
        if (p.value(QStringLiteral("active_window_id")).isDouble()) {
            ev.hasId = true;
            ev.id = static_cast<quint64>(p.value(QStringLiteral("active_window_id")).toDouble());
        }
        break;

    case EventKind::WindowsChanged:
        if (p.value(QStringLiteral("windows")).isArray()) {
            for (const QJsonValue &v : p.value(QStringLiteral("windows")).toArray()) {
                if (v.isObject()) {
                    ev.windows.append(WindowInfo::fromJson(v.toObject()));
                }
            }
        }
        break;

    case EventKind::WindowOpenedOrChanged:
        if (p.value(QStringLiteral("window")).isObject()) {
            ev.window = WindowInfo::fromJson(p.value(QStringLiteral("window")).toObject());
        } else {
            // A variant whose payload we cannot read is dropped by the caller
            // because window.id == 0.
            ev.window.id = 0;
        }
        break;

    case EventKind::WindowClosed:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        break;

    case EventKind::WindowFocusChanged:
        if (p.value(QStringLiteral("id")).isDouble()) {
            ev.hasId = true;
            ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        } else {
            // "None" means no window is focused.
            ev.hasId = false;
        }
        break;

    case EventKind::WindowFocusTimestampChanged:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        if (p.value(QStringLiteral("focus_timestamp")).isObject()) {
            const QJsonObject ts = p.value(QStringLiteral("focus_timestamp")).toObject();
            ev.hasTimestamp = true;
            ev.timestampSecs = static_cast<quint64>(ts.value(QStringLiteral("secs")).toDouble());
            ev.timestampNanos = static_cast<quint32>(ts.value(QStringLiteral("nanos")).toDouble());
        }
        break;

    case EventKind::WindowUrgencyChanged:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("id")).toDouble());
        ev.urgent = p.value(QStringLiteral("urgent")).toBool();
        break;

    case EventKind::WindowLayoutsChanged:
        if (p.value(QStringLiteral("changes")).isArray()) {
            for (const QJsonValue &v : p.value(QStringLiteral("changes")).toArray()) {
                if (!v.isArray()) {
                    continue;
                }
                const auto pair = v.toArray();
                if (pair.size() != 2) {
                    continue;
                }
                // [id, layout] -- an id/layout tuple, not an object.
                if (pair.at(0).isDouble() && pair.at(1).isObject()) {
                    ev.layoutChanges.append(qMakePair(
                        static_cast<quint64>(pair.at(0).toDouble()),
                        WindowLayout::fromJson(pair.at(1).toObject())));
                }
            }
        }
        break;

    case EventKind::KeyboardLayoutsChanged:
        if (p.value(QStringLiteral("keyboard_layouts")).isObject()) {
            ev.keyboardLayouts =
                KeyboardLayouts::fromJson(p.value(QStringLiteral("keyboard_layouts")).toObject());
        }
        break;

    case EventKind::KeyboardLayoutSwitched:
        ev.index = p.value(QStringLiteral("idx")).toInt();
        break;

    case EventKind::OverviewOpenedOrClosed:
        ev.isOpen = p.value(QStringLiteral("is_open")).toBool();
        break;

    case EventKind::ConfigLoaded:
        ev.failed = p.value(QStringLiteral("failed")).toBool();
        if (ev.failed) {
            LOG_WARN(Niri, "niri reported a failed config load");
        }
        break;

    case EventKind::ScreenshotCaptured:
        if (p.value(QStringLiteral("path")).isString()) {
            ev.hasPath = true;
            ev.path = p.value(QStringLiteral("path")).toString();
        }
        break;

    case EventKind::CastsChanged:
        if (p.value(QStringLiteral("casts")).isArray()) {
            for (const QJsonValue &v : p.value(QStringLiteral("casts")).toArray()) {
                if (v.isObject()) {
                    ev.casts.append(CastInfo::fromJson(v.toObject()));
                }
            }
        }
        break;

    case EventKind::CastStartedOrChanged:
        if (p.value(QStringLiteral("cast")).isObject()) {
            ev.casts.append(CastInfo::fromJson(p.value(QStringLiteral("cast")).toObject()));
        }
        break;

    case EventKind::CastStopped:
        ev.id = static_cast<quint64>(p.value(QStringLiteral("stream_id")).toDouble());
        break;

    case EventKind::Unknown:
        break;
    }

    return ev;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

namespace {

struct ActionName {
    ActionId id;
    const char *name;
};

// Transcribed from niri-ipc's `pub enum Action`. The spelling is load-bearing:
// serde will not deserialise a name that does not match.
constexpr ActionName kActionNames[] = {
    { ActionId::Quit, "Quit" },
    { ActionId::PowerOffMonitors, "PowerOffMonitors" },
    { ActionId::PowerOnMonitors, "PowerOnMonitors" },
    { ActionId::CloseWindow, "CloseWindow" },
    { ActionId::FullscreenWindow, "FullscreenWindow" },
    { ActionId::ToggleWindowedFullscreen, "ToggleWindowedFullscreen" },
    { ActionId::FocusWindow, "FocusWindow" },
    { ActionId::FocusWindowInColumn, "FocusWindowInColumn" },
    { ActionId::FocusWindowPrevious, "FocusWindowPrevious" },
    { ActionId::FocusColumnLeft, "FocusColumnLeft" },
    { ActionId::FocusColumnRight, "FocusColumnRight" },
    { ActionId::FocusColumnFirst, "FocusColumnFirst" },
    { ActionId::FocusColumnLast, "FocusColumnLast" },
    { ActionId::FocusColumnRightOrFirst, "FocusColumnRightOrFirst" },
    { ActionId::FocusColumnLeftOrLast, "FocusColumnLeftOrLast" },
    { ActionId::FocusColumn, "FocusColumn" },
    { ActionId::FocusWindowOrMonitorUp, "FocusWindowOrMonitorUp" },
    { ActionId::FocusWindowOrMonitorDown, "FocusWindowOrMonitorDown" },
    { ActionId::FocusWindowDown, "FocusWindowDown" },
    { ActionId::FocusWindowUp, "FocusWindowUp" },
    { ActionId::FocusWindowDownOrColumnLeft, "FocusWindowDownOrColumnLeft" },
    { ActionId::FocusWindowDownOrColumnRight, "FocusWindowDownOrColumnRight" },
    { ActionId::FocusWindowUpOrColumnLeft, "FocusWindowUpOrColumnLeft" },
    { ActionId::FocusWindowUpOrColumnRight, "FocusWindowUpOrColumnRight" },
    { ActionId::FocusWindowOrWorkspaceDown, "FocusWindowOrWorkspaceDown" },
    { ActionId::FocusWindowOrWorkspaceUp, "FocusWindowOrWorkspaceUp" },
    { ActionId::FocusWindowTop, "FocusWindowTop" },
    { ActionId::FocusWindowBottom, "FocusWindowBottom" },
    { ActionId::FocusWindowUpOrBottom, "FocusWindowUpOrBottom" },
    { ActionId::MoveWindowUp, "MoveWindowUp" },
    { ActionId::MoveWindowDown, "MoveWindowDown" },
    { ActionId::ConsumeOrExpelWindowLeft, "ConsumeOrExpelWindowLeft" },
    { ActionId::ConsumeOrExpelWindowRight, "ConsumeOrExpelWindowRight" },
    { ActionId::ConsumeWindowIntoColumn, "ConsumeWindowIntoColumn" },
    { ActionId::ExpelWindowFromColumn, "ExpelWindowFromColumn" },
    { ActionId::SwapWindowRight, "SwapWindowRight" },
    { ActionId::SwapWindowLeft, "SwapWindowLeft" },
    { ActionId::ToggleColumnTabbedDisplay, "ToggleColumnTabbedDisplay" },
    { ActionId::SetColumnDisplay, "SetColumnDisplay" },
    { ActionId::CenterColumn, "CenterColumn" },
    { ActionId::CenterWindow, "CenterWindow" },
    { ActionId::CenterVisibleColumns, "CenterVisibleColumns" },
    { ActionId::MaximizeColumn, "MaximizeColumn" },
    { ActionId::MaximizeWindowToEdges, "MaximizeWindowToEdges" },
    { ActionId::SwitchPresetColumnWidth, "SwitchPresetColumnWidth" },
    { ActionId::SwitchPresetColumnWidthBack, "SwitchPresetColumnWidthBack" },
    { ActionId::SwitchPresetWindowWidth, "SwitchPresetWindowWidth" },
    { ActionId::SwitchPresetWindowWidthBack, "SwitchPresetWindowWidthBack" },
    { ActionId::SwitchPresetWindowHeight, "SwitchPresetWindowHeight" },
    { ActionId::SwitchPresetWindowHeightBack, "SwitchPresetWindowHeightBack" },
    { ActionId::ExpandColumnToAvailableWidth, "ExpandColumnToAvailableWidth" },
    { ActionId::ToggleWindowFloating, "ToggleWindowFloating" },
    { ActionId::MoveWindowToFloating, "MoveWindowToFloating" },
    { ActionId::MoveWindowToTiling, "MoveWindowToTiling" },
    { ActionId::FocusFloating, "FocusFloating" },
    { ActionId::FocusTiling, "FocusTiling" },
    { ActionId::SwitchFocusBetweenFloatingAndTiling, "SwitchFocusBetweenFloatingAndTiling" },
    { ActionId::ToggleWindowUrgent, "ToggleWindowUrgent" },
    { ActionId::SetWindowUrgent, "SetWindowUrgent" },
    { ActionId::UnsetWindowUrgent, "UnsetWindowUrgent" },
    { ActionId::FocusWorkspaceDown, "FocusWorkspaceDown" },
    { ActionId::FocusWorkspaceUp, "FocusWorkspaceUp" },
    { ActionId::FocusWorkspace, "FocusWorkspace" },
    { ActionId::FocusWorkspacePrevious, "FocusWorkspacePrevious" },
    { ActionId::MoveWindowToWorkspaceDown, "MoveWindowToWorkspaceDown" },
    { ActionId::MoveWindowToWorkspaceUp, "MoveWindowToWorkspaceUp" },
    { ActionId::MoveWindowToWorkspace, "MoveWindowToWorkspace" },
    { ActionId::MoveColumnToWorkspaceDown, "MoveColumnToWorkspaceDown" },
    { ActionId::MoveColumnToWorkspaceUp, "MoveColumnToWorkspaceUp" },
    { ActionId::MoveColumnToWorkspace, "MoveColumnToWorkspace" },
    { ActionId::MoveWorkspaceDown, "MoveWorkspaceDown" },
    { ActionId::MoveWorkspaceUp, "MoveWorkspaceUp" },
    { ActionId::MoveWorkspaceToIndex, "MoveWorkspaceToIndex" },
    { ActionId::SetWorkspaceName, "SetWorkspaceName" },
    { ActionId::UnsetWorkspaceName, "UnsetWorkspaceName" },
    { ActionId::FocusMonitorLeft, "FocusMonitorLeft" },
    { ActionId::FocusMonitorRight, "FocusMonitorRight" },
    { ActionId::FocusMonitorDown, "FocusMonitorDown" },
    { ActionId::FocusMonitorUp, "FocusMonitorUp" },
    { ActionId::FocusMonitorPrevious, "FocusMonitorPrevious" },
    { ActionId::FocusMonitorNext, "FocusMonitorNext" },
    { ActionId::FocusMonitor, "FocusMonitor" },
    { ActionId::MoveWindowToMonitorLeft, "MoveWindowToMonitorLeft" },
    { ActionId::MoveWindowToMonitorRight, "MoveWindowToMonitorRight" },
    { ActionId::MoveWindowToMonitorDown, "MoveWindowToMonitorDown" },
    { ActionId::MoveWindowToMonitorUp, "MoveWindowToMonitorUp" },
    { ActionId::MoveWindowToMonitorPrevious, "MoveWindowToMonitorPrevious" },
    { ActionId::MoveWindowToMonitorNext, "MoveWindowToMonitorNext" },
    { ActionId::MoveWindowToMonitor, "MoveWindowToMonitor" },
    { ActionId::MoveColumnToMonitorLeft, "MoveColumnToMonitorLeft" },
    { ActionId::MoveColumnToMonitorRight, "MoveColumnToMonitorRight" },
    { ActionId::MoveColumnToMonitorDown, "MoveColumnToMonitorDown" },
    { ActionId::MoveColumnToMonitorUp, "MoveColumnToMonitorUp" },
    { ActionId::MoveColumnToMonitorPrevious, "MoveColumnToMonitorPrevious" },
    { ActionId::MoveColumnToMonitorNext, "MoveColumnToMonitorNext" },
    { ActionId::MoveColumnToMonitor, "MoveColumnToMonitor" },
    { ActionId::MoveWorkspaceToMonitorLeft, "MoveWorkspaceToMonitorLeft" },
    { ActionId::MoveWorkspaceToMonitorRight, "MoveWorkspaceToMonitorRight" },
    { ActionId::MoveWorkspaceToMonitorDown, "MoveWorkspaceToMonitorDown" },
    { ActionId::MoveWorkspaceToMonitorUp, "MoveWorkspaceToMonitorUp" },
    { ActionId::MoveWorkspaceToMonitorPrevious, "MoveWorkspaceToMonitorPrevious" },
    { ActionId::MoveWorkspaceToMonitorNext, "MoveWorkspaceToMonitorNext" },
    { ActionId::MoveWorkspaceToMonitor, "MoveWorkspaceToMonitor" },
    { ActionId::SetWindowWidth, "SetWindowWidth" },
    { ActionId::SetWindowHeight, "SetWindowHeight" },
    { ActionId::ResetWindowHeight, "ResetWindowHeight" },
    { ActionId::SetColumnWidth, "SetColumnWidth" },
    { ActionId::SwitchLayout, "SwitchLayout" },
    { ActionId::ShowHotkeyOverlay, "ShowHotkeyOverlay" },
    { ActionId::ToggleOverview, "ToggleOverview" },
    { ActionId::OpenOverview, "OpenOverview" },
    { ActionId::CloseOverview, "CloseOverview" },
    { ActionId::Spawn, "Spawn" },
    { ActionId::SpawnSh, "SpawnSh" },
    { ActionId::Screenshot, "Screenshot" },
    { ActionId::ScreenshotScreen, "ScreenshotScreen" },
    { ActionId::ScreenshotWindow, "ScreenshotWindow" },
    { ActionId::ToggleKeyboardShortcutsInhibit, "ToggleKeyboardShortcutsInhibit" },
    { ActionId::LoadConfigFile, "LoadConfigFile" },
    { ActionId::StopCast, "StopCast" },
    { ActionId::SetDynamicCastWindow, "SetDynamicCastWindow" },
    { ActionId::SetDynamicCastMonitor, "SetDynamicCastMonitor" },
    { ActionId::ClearDynamicCastTarget, "ClearDynamicCastTarget" },
    { ActionId::ToggleDebugTint, "ToggleDebugTint" },
    { ActionId::DebugToggleOpaqueRegions, "DebugToggleOpaqueRegions" },
    { ActionId::DebugToggleDamage, "DebugToggleDamage" },
    { ActionId::ToggleWindowRuleOpacity, "ToggleWindowRuleOpacity" },
};

// Actions whose struct variant carries only `id: Option<u64>`.
bool hasOptionalId(ActionId id)
{
    switch (id) {
    case ActionId::FocusWindow:
    case ActionId::FocusWindowInColumn:
    case ActionId::FocusWindowPrevious:
    case ActionId::FocusColumn:
    case ActionId::FocusWindowDown:
    case ActionId::FocusWindowUp:
    case ActionId::FocusWindowDownOrColumnLeft:
    case ActionId::FocusWindowDownOrColumnRight:
    case ActionId::FocusWindowUpOrColumnLeft:
    case ActionId::FocusWindowUpOrColumnRight:
    case ActionId::FocusWindowOrWorkspaceDown:
    case ActionId::FocusWindowOrWorkspaceUp:
    case ActionId::FocusWindowTop:
    case ActionId::FocusWindowBottom:
    case ActionId::FocusWindowUpOrBottom:
    case ActionId::MoveWindowUp:
    case ActionId::MoveWindowDown:
    case ActionId::ConsumeOrExpelWindowLeft:
    case ActionId::ConsumeOrExpelWindowRight:
    case ActionId::ConsumeWindowIntoColumn:
    case ActionId::ExpelWindowFromColumn:
    case ActionId::SwapWindowRight:
    case ActionId::SwapWindowLeft:
    case ActionId::CenterWindow:
    case ActionId::MaximizeWindowToEdges:
    case ActionId::ExpandColumnToAvailableWidth:
    case ActionId::ToggleWindowFloating:
    case ActionId::MoveWindowToFloating:
    case ActionId::MoveWindowToTiling:
    case ActionId::ToggleWindowUrgent:
    case ActionId::SetWindowUrgent:
    case ActionId::UnsetWindowUrgent:
    case ActionId::FocusWorkspace:
    case ActionId::MoveWindowToWorkspace:
    case ActionId::SetWorkspaceName:
    case ActionId::FocusMonitor:
    case ActionId::MoveWindowToMonitor:
    case ActionId::MoveColumnToMonitor:
    case ActionId::MoveWorkspaceToMonitor:
    case ActionId::SetDynamicCastWindow:
    case ActionId::ToggleWindowRuleOpacity:
    case ActionId::StopCast:
    case ActionId::SetWindowWidth:
    case ActionId::SetWindowHeight:
    case ActionId::ResetWindowHeight:
        return true;
    default:
        return false;
    }
}

} // namespace

const char *actionIdToString(ActionId id)
{
    for (const ActionName &a : kActionNames) {
        if (a.id == id) {
            return a.name;
        }
    }
    return nullptr;
}

QJsonObject buildAction(ActionId id, bool hasTargetId, quint64 targetId, const QJsonObject &extra)
{
    QJsonObject obj;
    const char *name = actionIdToString(id);
    if (name == nullptr) {
        LOG_WARNF(Niri, "action id %d has no wire name; refusing to send it",
            static_cast<int>(id));
        return obj;
    }
    obj.insert(QString::fromLatin1(name), extra);

    if (hasOptionalId(id)) {
        // `id: Option<u64>` with no explicit id must serialise as `null`, which
        // tells niri "the focused one".
        if (hasTargetId) {
            obj.insert(QStringLiteral("id"), static_cast<double>(targetId));
        } else {
            obj.insert(QStringLiteral("id"), QJsonValue());
        }
    }
    return obj;
}

QJsonObject actionFocusWorkspace(quint64 workspaceId)
{
    return buildAction(ActionId::FocusWorkspace, true, workspaceId);
}

QJsonObject actionFocusWindow(quint64 windowId)
{
    return buildAction(ActionId::FocusWindow, true, windowId);
}

QJsonObject actionCloseWindow(quint64 windowId)
{
    return buildAction(ActionId::CloseWindow, true, windowId);
}

QJsonObject actionFocusWorkspaceIndex(int index, bool hasIndex)
{
    QJsonObject extra;
    if (hasIndex) {
        // FocusWorkspace takes a WorkspaceReferenceArg, an externally tagged
        // enum: {"Index": 2} / {"Id": 7} / {"Name": "web"}.
        extra.insert(QStringLiteral("reference"), QJsonObject {
            { QStringLiteral("Index"), static_cast<double>(index) }
        });
    }
    return buildAction(ActionId::FocusWorkspace, false, 0, extra);
}

QJsonObject actionFocusMonitor(const QString &outputName)
{
    if (outputName.isEmpty()) {
        return buildAction(ActionId::FocusMonitor);
    }
    QJsonObject extra;
    extra.insert(QStringLiteral("output"), outputName);
    return buildAction(ActionId::FocusMonitor, false, 0, extra);
}

QJsonObject actionSwitchLayout(const QString &target)
{
    // LayoutSwitchTarget: "next" | "prev" | {"Index": 2}
    QJsonObject value;
    if (target.compare(QLatin1String("next"), Qt::CaseInsensitive) == 0
        || target.compare(QLatin1String("prev"), Qt::CaseInsensitive) == 0) {
        value = target.toLower();
    } else {
        value.insert(QStringLiteral("Index"), target.toDouble());
    }
    QJsonObject extra;
    extra.insert(QStringLiteral("layout"), value);
    return buildAction(ActionId::SwitchLayout, false, 0, extra);
}

QJsonObject actionQuit(bool skipConfirmation)
{
    QJsonObject extra;
    extra.insert(QStringLiteral("skip_confirmation"), skipConfirmation);
    return buildAction(ActionId::Quit, false, 0, extra);
}

// ---------------------------------------------------------------------------
// VersionSupport
// ---------------------------------------------------------------------------

VersionSupport VersionSupport::parse(const QString &raw)
{
    VersionSupport v;
    v.raw = raw;

    // niri reports e.g. "26.4.0" or "25.11 (git sha 1a2b3c4)". Take the leading
    // dotted number.
    static const QRegularExpression re(QStringLiteral("^(\\d+)(?:\\.(\\d+))?(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = re.match(raw);
    if (!m.hasMatch()) {
        v.note = QStringLiteral("could not parse a version number; assuming compatible");
        return v;
    }
    v.parsed = true;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    v.patch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();

    const int minMajor = 25;
    const int minMinor = 11;
    const int testedMajor = 26;
    const int testedMinor = 4;

    if (v.major < minMajor || (v.major == minMajor && v.minor < minMinor)) {
        v.belowMinimum = true;
        v.note = QStringLiteral("older than the supported minimum %1")
                     .arg(QLatin1String(KAPAH_NIRI_MIN_VERSION));
    } else if (v.major > testedMajor || (v.major == testedMajor && v.minor > testedMinor)) {
        v.aboveTested = true;
        v.note = QStringLiteral("newer than the version the fixtures were recorded on (%1); "
                                "unknown events will be ignored")
                     .arg(QLatin1String(KAPAH_NIRI_TESTED_VERSION));
    }
    return v;
}

} // namespace kapah::niri