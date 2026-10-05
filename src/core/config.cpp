#include "config.h"

#include "env.h"
#include "logging.h"
#include "paths.h"
#include "kapah/version.h"

#include <QFileInfo>

namespace kapah {

namespace {

struct ModuleName {
    BarModuleId id;
    const char *name;
};

constexpr ModuleName kModules[] = {
    { BarModuleId::Workspaces, "workspaces" },
    { BarModuleId::WindowTitle, "window-title" },
    { BarModuleId::Clock, "clock" },
    { BarModuleId::Battery, "battery" },
    { BarModuleId::Network, "network" },
    { BarModuleId::Audio, "audio" },
    { BarModuleId::Backlight, "backlight" },
    { BarModuleId::Tray, "tray" },
    { BarModuleId::Mpris, "mpris" },
    { BarModuleId::Layout, "layout" },
    { BarModuleId::Session, "session" },
};

QVector<BarModuleId> parseModuleList(const QString &csv, bool *ok)
{
    QVector<BarModuleId> out;
    *ok = true;
    const auto parts = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        BarModuleId id;
        const QString name = raw.trimmed();
        if (!barModuleIdFromString(name, &id)) {
            *ok = false;
            out.append(BarModuleId::Clock); // keep parsing; caller warns
            continue;
        }
        if (!out.contains(id)) {
            out.append(id);
        }
    }
    return out;
}

BarSectionConfig parseSection(const toml::ValuePtr &v, const BarSectionConfig &fallback,
    QVector<toml::Diagnostic> *diags, const QString &where)
{
    if (!v || !v->isTable()) {
        return fallback;
    }
    const auto table = v->toTable();

    auto it = table.constFind(QStringLiteral("modules"));
    if (it == table.constEnd()) {
        return fallback;
    }
    const toml::ValuePtr &mv = it.value();
    QVector<BarModuleId> ids;

    if (mv->isString()) {
        bool ok = false;
        ids = parseModuleList(mv->toString(), &ok);
        if (!ok) {
            diags->append({ toml::Diagnostic::Warning, mv->line(), mv->column(),
                QStringLiteral("unknown bar module in %1.modules").arg(where) });
        }
    } else if (mv->isArray()) {
        for (const auto &item : mv->toArray()) {
            BarModuleId id;
            if (item->isString() && barModuleIdFromString(item->toString(), &id)) {
                if (!ids.contains(id)) {
                    ids.append(id);
                }
            } else {
                diags->append({ toml::Diagnostic::Warning, item->line(), item->column(),
                    QStringLiteral("unknown bar module in %1.modules").arg(where) });
            }
        }
    } else {
        diags->append({ toml::Diagnostic::Warning, mv->line(), mv->column(),
            QStringLiteral("%1.modules must be a string or an array of strings").arg(where) });
    }

    BarSectionConfig out = fallback;
    out.modules = ids;
    return out;
}

QHash<QString, QString> parseStringMap(const toml::ValuePtr &v,
    QVector<toml::Diagnostic> *diags, const QString &where)
{
    QHash<QString, QString> out;
    if (!v || !v->isTable()) {
        return out;
    }
    const auto table = v->toTable();
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        if (it.value()->isString()) {
            out.insert(it.key(), it.value()->toString());
        } else if (it.value()->isTable()) {
            // [bar.per-output.DP-1] style, handled by the caller.
            continue;
        } else {
            diags->append({ toml::Diagnostic::Warning, it.value()->line(), it.value()->column(),
                QStringLiteral("%1.%2 must be a string").arg(where, it.key()) });
        }
    }
    return out;
}

QColor parseColor(const toml::ValuePtr &v, const QColor &fallback, const QString &where,
    QVector<toml::Diagnostic> *diags)
{
    if (!v) {
        return fallback;
    }
    const QString s = v->toString();
    if (s.isEmpty()) {
        return fallback;
    }
    QColor c(s);
    if (!c.isValid()) {
        diags->append({ toml::Diagnostic::Warning, v->line(), v->column(),
            QStringLiteral("%1: '%2' is not a valid colour").arg(where, s) });
        return fallback;
    }
    return c;
}

const toml::ValuePtr child(const toml::ValuePtr &v, const char *key)
{
    if (!v || !v->isTable()) {
        return nullptr;
    }
    const auto table = v->toTable();
    const auto it = table.constFind(QString::fromLatin1(key));
    return it == table.constEnd() ? nullptr : it.value();
}

QMargins parseMargins(const toml::ValuePtr &v, const QMargins &fallback)
{
    if (!v) {
        return fallback;
    }
    if (v->isInteger()) {
        const int m = static_cast<int>(v->toInteger());
        return QMargins(m, m, m, m);
    }
    if (v->isArray()) {
        const auto a = v->toArray();
        // [top, right, bottom, left] in CSS order; 2 entries = vertical,horizontal.
        if (a.size() == 4) {
            return QMargins(static_cast<int>(a[3]->toInteger()),
                static_cast<int>(a[1]->toInteger()),
                static_cast<int>(a[2]->toInteger()),
                static_cast<int>(a[0]->toInteger()));
        }
        if (a.size() == 2) {
            const int v1 = static_cast<int>(a[0]->toInteger());
            const int v2 = static_cast<int>(a[1]->toInteger());
            return QMargins(v1, v2, v1, v2);
        }
    }
    return fallback;
}

} // namespace

QString barModuleIdToString(BarModuleId id)
{
    for (const auto &m : kModules) {
        if (m.id == id) {
            return QString::fromLatin1(m.name);
        }
    }
    return QStringLiteral("unknown");
}

bool barModuleIdFromString(const QString &s, BarModuleId *out)
{
    const QString needle = s.trimmed().toLower();
    for (const auto &m : kModules) {
        if (needle == QLatin1String(m.name)) {
            *out = m.id;
            return true;
        }
    }
    // A couple of aliases so a typo in a shared config does not silently drop
    // the module.
    if (needle == QLatin1String("windowtitle")) {
        *out = BarModuleId::WindowTitle;
        return true;
    }
    if (needle == QLatin1String("title")) {
        *out = BarModuleId::WindowTitle;
        return true;
    }
    if (needle == QLatin1String("wifi")) {
        *out = BarModuleId::Network;
        return true;
    }
    if (needle == QLatin1String("volume")) {
        *out = BarModuleId::Audio;
        return true;
    }
    if (needle == QLatin1String("keyboard-layout")) {
        *out = BarModuleId::Layout;
        return true;
    }
    return false;
}

QStringList allBarModuleIds()
{
    QStringList out;
    for (const auto &m : kModules) {
        out.append(QString::fromLatin1(m.name));
    }
    return out;
}

BarSectionConfig BarSectionConfig::defaults()
{
    // Empty by default; Config::defaults() fills in the three real sections.
    return BarSectionConfig {};
}

Config Config::defaults()
{
    Config c;
    c.revision = 0;

    c.bar.enabled = true;
    c.bar.position = BarPosition::Top;
    c.bar.visibility = BarVisibility::Always;
    c.bar.height = 28;

    c.bar.left.modules = { BarModuleId::Workspaces };
    c.bar.center.modules = { BarModuleId::WindowTitle };
    c.bar.right.modules = { BarModuleId::Layout, BarModuleId::Network, BarModuleId::Audio,
        BarModuleId::Battery, BarModuleId::Clock, BarModuleId::Tray, BarModuleId::Session };

    return c;
}

namespace {

bool qMarginsEq(const QMargins &a, const QMargins &b)
{
    return a.left() == b.left() && a.top() == b.top() && a.right() == b.right()
        && a.bottom() == b.bottom();
}

bool qColorEq(const QColor &a, const QColor &b)
{
    return a == b;
}

} // namespace

bool Config::generalEquals(const Config &o) const
{
    return general.scale == o.general.scale && general.fontFamily == o.general.fontFamily
        && general.theme == o.general.theme && general.animations == o.general.animations
        && general.compact == o.general.compact && general.reduceMotion == o.general.reduceMotion
        && general.locale == o.general.locale;
}

bool Config::themeEquals(const Config &o) const
{
    return theme.radius == o.theme.radius && theme.radiusSmall == o.theme.radiusSmall
        && theme.radiusLarge == o.theme.radiusLarge && theme.borderWidth == o.theme.borderWidth
        && theme.opacity == o.theme.opacity
        && theme.overrides == o.theme.overrides;
}

namespace {

bool barSectionEq(const BarSectionConfig &a, const BarSectionConfig &b)
{
    return a.modules == b.modules;
}

bool barConfigEq(const BarConfig &a, const BarConfig &b)
{
    if (a.enabled != b.enabled || a.position != b.position || a.visibility != b.visibility
        || a.height != b.height || !qMarginsEq(a.margins, b.margins) || a.spacing != b.spacing
        || a.padding != b.padding || a.autoHideOnFullscreen != b.autoHideOnFullscreen) {
        return false;
    }
    if (!barSectionEq(a.left, b.left) || !barSectionEq(a.center, b.center)
        || !barSectionEq(a.right, b.right)) {
        return false;
    }
    if (a.perOutput.size() != b.perOutput.size()) {
        return false;
    }
    for (auto it = a.perOutput.constBegin(); it != a.perOutput.constEnd(); ++it) {
        const auto other = b.perOutput.constFind(it.key());
        if (other == b.perOutput.constEnd()) {
            return false;
        }
        if (!barConfigEq(it.value(), other.value())) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Config::barEquals(const Config &o) const
{
    return barConfigEq(bar, o.bar);
}

bool Config::notificationsEqual(const Config &o) const
{
    const NotificationConfig &a = notifications;
    const NotificationConfig &b = o.notifications;
    return a.daemonEnabled == b.daemonEnabled && a.timeoutMs == b.timeoutMs
        && a.defaultTimeoutOverride == b.defaultTimeoutOverride && a.maxVisible == b.maxVisible
        && a.historySize == b.historySize && a.historyBodyMaxChars == b.historyBodyMaxChars
        && a.historyImageMaxPx == b.historyImageMaxPx && a.groupBursts == b.groupBursts
        && a.burstWindowMs == b.burstWindowMs && a.perAppRateLimit == b.perAppRateLimit
        && a.position == b.position && a.margin == b.margin && a.width == b.width
        && a.doNotDisturb == b.doNotDisturb && a.perAppRules == b.perAppRules
        && a.persistHistoryOnExit == b.persistHistoryOnExit && a.panelEnabled == b.panelEnabled
        && a.panelWidth == b.panelWidth && a.panelHeight == b.panelHeight;
}

bool Config::launcherEquals(const Config &o) const
{
    const LauncherConfig &a = launcher;
    const LauncherConfig &b = o.launcher;
    return a.enabled == b.enabled && a.width == b.width && a.maxResults == b.maxResults
        && a.iconSize == b.iconSize && a.destroyGraceMs == b.destroyGraceMs && a.terminal == b.terminal
        && a.launchMethod == b.launchMethod && a.trimDelayMs == b.trimDelayMs
        && a.showKeywords == b.showKeywords && a.showGenericName == b.showGenericName
        && a.calculatorEnabled == b.calculatorEnabled
        && a.commandRunnerEnabled == b.commandRunnerEnabled
        && a.windowSwitcherEnabled == b.windowSwitcherEnabled && a.clipboardEnabled == b.clipboardEnabled
        && a.emojiEnabled == b.emojiEnabled && a.frecencySize == b.frecencySize;
}

bool Config::osdEquals(const Config &o) const
{
    const OsdConfig &a = osd;
    const OsdConfig &b = o.osd;
    return a.enabled == b.enabled && a.timeoutMs == b.timeoutMs && a.position == b.position
        && a.width == b.width && a.height == b.height
        && a.suppressAfterStartupMs == b.suppressAfterStartupMs
        && a.showOnKeypress == b.showOnKeypress && a.showBrightness == b.showBrightness;
}

bool Config::quickSettingsEqual(const Config &o) const
{
    const QuickSettingsConfig &a = quickSettings;
    const QuickSettingsConfig &b = o.quickSettings;
    return a.enabled == b.enabled && a.width == b.width && a.destroyGraceMs == b.destroyGraceMs
        && a.showBluetooth == b.showBluetooth && a.showWifiList == b.showWifiList
        && a.showPowerProfile == b.showPowerProfile && a.showNightLight == b.showNightLight
        && a.showBatteryDetails == b.showBatteryDetails && a.scanOnOpen == b.scanOnOpen
        && a.volumeStep == b.volumeStep && a.brightnessStep == b.brightnessStep;
}

bool Config::lockEquals(const Config &o) const
{
    const LockConfig &a = lock;
    const LockConfig &b = o.lock;
    return a.enabled == b.enabled && a.graceSeconds == b.graceSeconds
        && a.lockOnSuspend == b.lockOnSuspend && a.lockOnLidClose == b.lockOnLidClose
        && a.wrongPasswordDelayMs == b.wrongPasswordDelayMs
        && a.retryLockoutMs == b.retryLockoutMs && a.retryLockoutAfter == b.retryLockoutAfter
        && a.showClock == b.showClock && a.showKeyboardLayout == b.showKeyboardLayout
        && a.background == b.background && qColorEq(a.backgroundColor, b.backgroundColor);
}

bool Config::idleEquals(const Config &o) const
{
    const IdleConfig &a = idle;
    const IdleConfig &b = o.idle;
    return a.dimSeconds == b.dimSeconds && a.dimPercent == b.dimPercent
        && a.lockSeconds == b.lockSeconds && a.dpmsOffSeconds == b.dpmsOffSeconds
        && a.suspendSeconds == b.suspendSeconds
        && a.inhibitWhilePlaying == b.inhibitWhilePlaying
        && a.inhibitOnFullscreen == b.inhibitOnFullscreen && a.inhibitWhileBusy == b.inhibitWhileBusy
        && a.resumeGraceSeconds == b.resumeGraceSeconds;
}

bool Config::wallpaperEquals(const Config &o) const
{
    return wallpaper.mode == o.wallpaper.mode && wallpaper.path == o.wallpaper.path
        && qColorEq(wallpaper.color, o.wallpaper.color)
        && wallpaper.maxRssKbPerOutput == o.wallpaper.maxRssKbPerOutput;
}

bool Config::trayEquals(const Config &o) const
{
    const TrayConfig &a = tray;
    const TrayConfig &b = o.tray;
    return a.enabled == b.enabled && a.iconSize == b.iconSize && a.spacing == b.spacing
        && a.maxItems == b.maxItems && a.menuOnLeftClick == b.menuOnLeftClick
        && a.contextMenus == b.contextMenus;
}

bool Config::mprisEquals(const Config &o) const
{
    return mpris.enabled == o.mpris.enabled && mpris.maxTrackChars == o.mpris.maxTrackChars;
}

bool Config::bluetoothEquals(const Config &o) const
{
    return bluetooth.enabled == o.bluetooth.enabled
        && bluetooth.scanTimeoutMs == o.bluetooth.scanTimeoutMs
        && bluetooth.maxDevices == o.bluetooth.maxDevices;
}

bool Config::servicesEqual(const Config &o) const
{
    const ServiceConfig &a = services;
    const ServiceConfig &b = o.services;
    return a.networkBackends == b.networkBackends && a.audioBackend == b.audioBackend
        && a.upower == b.upower && a.udevBattery == b.udevBattery
        && a.nmSecretAgent == b.nmSecretAgent && a.nmWifiList == b.nmWifiList
        && a.perfLogging == b.perfLogging;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

Config configFromToml(const toml::Document &doc, const QString &sourceFile, const Config &previous,
    QVector<toml::Diagnostic> *diagnostics)
{
    Config c = previous;
    c.sourceFile = sourceFile;

    auto note = [diagnostics](toml::Diagnostic::Severity sev, const toml::ValuePtr &v,
                              const QString &msg) {
        const int line = v ? v->line() : 0;
        const int col = v ? v->column() : 0;
        diagnostics->append({ sev, line, col, msg });
    };

    if (!doc.root || !doc.root->isTable()) {
        return c;
    }
    const auto rootTable = doc.root->toTable();

    auto section = [&](const char *name) -> toml::ValuePtr {
        const auto it = rootTable.constFind(QString::fromLatin1(name));
        return it == rootTable.constEnd() ? nullptr : it.value();
    };

    auto readBool = [&](const toml::ValuePtr &s, const char *key, bool &field) {
        const auto v = child(s, key);
        if (v) {
            field = v->toBoolean(field);
        }
    };
    auto readInt = [&](const toml::ValuePtr &s, const char *key, int &field) {
        const auto v = child(s, key);
        if (v) {
            field = static_cast<int>(v->toInteger(field));
        }
    };
    auto readDouble = [&](const toml::ValuePtr &s, const char *key, double &field) {
        const auto v = child(s, key);
        if (v) {
            field = v->toFloat(field);
        }
    };
    auto readString = [&](const toml::ValuePtr &s, const char *key, QString &field) {
        const auto v = child(s, key);
        if (v && v->isString()) {
            field = v->toString();
        }
    };

    // ---- [general]
    if (const auto s = section("general")) {
        readDouble(s, "scale", c.general.scale);
        readString(s, "font-family", c.general.fontFamily);
        readString(s, "theme", c.general.theme);
        readBool(s, "animations", c.general.animations);
        readBool(s, "compact", c.general.compact);
        readBool(s, "reduce-motion", c.general.reduceMotion);
        readString(s, "locale", c.general.locale);
        if (c.general.scale < 0.5 || c.general.scale > 4.0) {
            note(toml::Diagnostic::Warning, child(s, "scale"),
                QStringLiteral("general.scale %1 is out of range [0.5, 4.0]; clamped")
                    .arg(c.general.scale));
            c.general.scale = qBound(0.5, c.general.scale, 4.0);
        }
    }

    // ---- [theme]
    if (const auto s = section("theme")) {
        readInt(s, "radius", c.theme.radius);
        readInt(s, "radius-small", c.theme.radiusSmall);
        readInt(s, "radius-large", c.theme.radiusLarge);
        readInt(s, "border-width", c.theme.borderWidth);
        readBool(s, "opacity", c.theme.opacity);
        const auto colors = child(s, "colors");
        if (colors && colors->isTable()) {
            c.theme.overrides = parseStringMap(colors, diagnostics,
                QStringLiteral("theme.colors"));
        }
        // The style shorthand is sugar for colors.bg / colors.fg.
        readString(s, "style", c.general.theme);
        if (const auto styleV = child(s, "style")) {
            const QString style = styleV->toString().toLower();
            if (style == QLatin1String("dark") || style == QLatin1String("light")
                || style == QLatin1String("system")) {
                c.general.theme = style;
            } else {
                note(toml::Diagnostic::Warning, styleV,
                    QStringLiteral("theme.style must be dark, light or system"));
            }
        }
    }

    // ---- [bar]
    if (const auto s = section("bar")) {
        readBool(s, "enabled", c.bar.enabled);
        readInt(s, "height", c.bar.height);
        readInt(s, "spacing", c.bar.spacing);
        readInt(s, "padding", c.bar.padding);
        readBool(s, "auto-hide-on-fullscreen", c.bar.autoHideOnFullscreen);

        if (c.bar.height < 16 || c.bar.height > 96) {
            note(toml::Diagnostic::Warning, child(s, "height"),
                QStringLiteral("bar.height %1 is out of range [16, 96]; clamped")
                    .arg(c.bar.height));
            c.bar.height = qBound(16, c.bar.height, 96);
        }

        if (const auto v = child(s, "margins")) {
            c.bar.margins = parseMargins(v, c.bar.margins);
        }

        if (const auto v = child(s, "position")) {
            const QString p = v->toString().toLower();
            if (p == QLatin1String("top")) {
                c.bar.position = BarPosition::Top;
            } else if (p == QLatin1String("bottom")) {
                c.bar.position = BarPosition::Bottom;
            } else {
                note(toml::Diagnostic::Warning, v,
                    QStringLiteral("bar.position must be 'top' or 'bottom'"));
            }
        }

        if (const auto v = child(s, "visibility")) {
            const QString vis = v->toString().toLower();
            if (vis == QLatin1String("always")) {
                c.bar.visibility = BarVisibility::Always;
            } else if (vis == QLatin1String("auto-hide")) {
                c.bar.visibility = BarVisibility::AutoHide;
            } else if (vis == QLatin1String("hidden")) {
                c.bar.visibility = BarVisibility::Hidden;
            } else {
                note(toml::Diagnostic::Warning, v,
                    QStringLiteral("bar.visibility must be always, auto-hide or hidden"));
            }
        }

        if (const auto left = child(s, "left")) {
            c.bar.left = parseSection(left, c.bar.left, diagnostics, QStringLiteral("bar.left"));
        }
        if (const auto center = child(s, "center")) {
            c.bar.center = parseSection(center, c.bar.center, diagnostics,
                QStringLiteral("bar.center"));
        }
        if (const auto right = child(s, "right")) {
            c.bar.right = parseSection(right, c.bar.right, diagnostics, QStringLiteral("bar.right"));
        }

        // [[bar.per-output]] array of tables, each optionally carrying every
        // [bar.per-output.N] key.
        const auto perOut = child(s, "per-output");
        if (perOut && perOut->isTable()) {
            for (auto it = perOut->toTable().constBegin(); it != perOut->toTable().constEnd();
                 ++it) {
                if (!it.value()->isTable()) {
                    continue;
                }
                const QString name = it.key();
                BarConfig sub = c.bar;
                sub.perOutput.clear();

                const auto t = it.value();
                readBool(t, "enabled", sub.enabled);
                readInt(t, "height", sub.height);
                readInt(t, "spacing", sub.spacing);
                readInt(t, "padding", sub.padding);
                readBool(t, "auto-hide-on-fullscreen", sub.autoHideOnFullscreen);
                if (const auto v = child(t, "margins")) {
                    sub.margins = parseMargins(v, sub.margins);
                }
                if (const auto v = child(t, "position")) {
                    sub.position = v->toString().toLower() == QLatin1String("bottom")
                        ? BarPosition::Bottom
                        : BarPosition::Top;
                }
                if (const auto v = child(t, "visibility")) {
                    const QString vis = v->toString().toLower();
                    sub.visibility = vis == QLatin1String("auto-hide") ? BarVisibility::AutoHide
                        : vis == QLatin1String("hidden")               ? BarVisibility::Hidden
                                                                       : BarVisibility::Always;
                }
                if (const auto l = child(t, "left")) {
                    sub.left = parseSection(l, sub.left, diagnostics,
                        QStringLiteral("bar.per-output.%1.left").arg(name));
                }
                if (const auto ce = child(t, "center")) {
                    sub.center = parseSection(ce, sub.center, diagnostics,
                        QStringLiteral("bar.per-output.%1.center").arg(name));
                }
                if (const auto r = child(t, "right")) {
                    sub.right = parseSection(r, sub.right, diagnostics,
                        QStringLiteral("bar.per-output.%1.right").arg(name));
                }

                // "output" key lets users write the output name inside the table
                // when the map key is awkward (contains a dash is fine either way).
                if (const auto v = child(t, "output")) {
                    c.bar.perOutput.insert(v->toString(), sub);
                } else {
                    c.bar.perOutput.insert(name, sub);
                }
            }
        }
    }

    // ---- [notifications]
    if (const auto s = section("notifications")) {
        readBool(s, "enabled", c.notifications.daemonEnabled);
        readInt(s, "timeout", c.notifications.timeoutMs);
        readInt(s, "default-timeout", c.notifications.defaultTimeoutOverride);
        readInt(s, "max-visible", c.notifications.maxVisible);
        readInt(s, "history-size", c.notifications.historySize);
        readInt(s, "history-body-max-chars", c.notifications.historyBodyMaxChars);
        readInt(s, "history-image-max-px", c.notifications.historyImageMaxPx);
        readBool(s, "group-bursts", c.notifications.groupBursts);
        readInt(s, "burst-window", c.notifications.burstWindowMs);
        readInt(s, "per-app-rate-limit", c.notifications.perAppRateLimit);
        readString(s, "position", c.notifications.position);
        readInt(s, "margin", c.notifications.margin);
        readInt(s, "width", c.notifications.width);
        readBool(s, "do-not-disturb", c.notifications.doNotDisturb);
        readBool(s, "persist-history", c.notifications.persistHistoryOnExit);
        readBool(s, "panel", c.notifications.panelEnabled);
        readInt(s, "panel-width", c.notifications.panelWidth);
        readInt(s, "panel-height", c.notifications.panelHeight);

        if (const auto rules = child(s, "rules")) {
            c.notifications.perAppRules = parseStringMap(rules, diagnostics,
                QStringLiteral("notifications.rules"));
            for (auto it = c.notifications.perAppRules.constBegin();
                 it != c.notifications.perAppRules.constEnd(); ++it) {
                const QString v = it.value();
                if (v != QLatin1String("mute") && v != QLatin1String("persist")) {
                    note(toml::Diagnostic::Warning, nullptr,
                        QStringLiteral("notifications.rules.%1: '%2' is not 'mute' or 'persist'")
                            .arg(it.key(), v));
                }
            }
        }

        const QString pos = c.notifications.position.toLower();
        if (pos != QLatin1String("top-right") && pos != QLatin1String("top-left")
            && pos != QLatin1String("bottom-right") && pos != QLatin1String("bottom-left")) {
            note(toml::Diagnostic::Warning, child(s, "position"),
                QStringLiteral("notifications.position must be one of top-right, top-left, "
                               "bottom-right, bottom-left"));
            c.notifications.position = QStringLiteral("top-right");
        }
        c.notifications.maxVisible = qBound(1, c.notifications.maxVisible, 10);
        c.notifications.historySize = qBound(0, c.notifications.historySize, 500);
        c.notifications.perAppRateLimit = qBound(0, c.notifications.perAppRateLimit, 600);
    }

    // ---- [launcher]
    if (const auto s = section("launcher")) {
        readBool(s, "enabled", c.launcher.enabled);
        readInt(s, "width", c.launcher.width);
        readInt(s, "max-results", c.launcher.maxResults);
        readInt(s, "icon-size", c.launcher.iconSize);
        readInt(s, "destroy-grace", c.launcher.destroyGraceMs);
        readString(s, "terminal", c.launcher.terminal);
        readString(s, "launch-method", c.launcher.launchMethod);
        readInt(s, "trim-delay", c.launcher.trimDelayMs);
        readBool(s, "show-keywords", c.launcher.showKeywords);
        readBool(s, "show-generic-name", c.launcher.showGenericName);
        readBool(s, "calculator", c.launcher.calculatorEnabled);
        readBool(s, "command-runner", c.launcher.commandRunnerEnabled);
        readBool(s, "window-switcher", c.launcher.windowSwitcherEnabled);
        readBool(s, "clipboard", c.launcher.clipboardEnabled);
        readBool(s, "emoji", c.launcher.emojiEnabled);
        readInt(s, "frecency-size", c.launcher.frecencySize);

        c.launcher.maxResults = qBound(3, c.launcher.maxResults, 20);
        c.launcher.iconSize = qBound(16, c.launcher.iconSize, 96);
        c.launcher.frecencySize = qBound(0, c.launcher.frecencySize, 4096);
        const QString method = c.launcher.launchMethod.toLower();
        if (method != QLatin1String("spawn") && method != QLatin1String("systemd")) {
            note(toml::Diagnostic::Warning, child(s, "launch-method"),
                QStringLiteral("launcher.launch-method must be 'spawn' or 'systemd'"));
            c.launcher.launchMethod = QStringLiteral("spawn");
        }
    }

    // ---- [osd]
    if (const auto s = section("osd")) {
        readBool(s, "enabled", c.osd.enabled);
        readInt(s, "timeout", c.osd.timeoutMs);
        readString(s, "position", c.osd.position);
        readInt(s, "width", c.osd.width);
        readInt(s, "height", c.osd.height);
        readInt(s, "suppress-after-startup", c.osd.suppressAfterStartupMs);
        readBool(s, "show-on-keypress", c.osd.showOnKeypress);
        readBool(s, "show-brightness", c.osd.showBrightness);
        const QString pos = c.osd.position.toLower();
        if (pos != QLatin1String("bottom-center") && pos != QLatin1String("top-center")
            && pos != QLatin1String("center")) {
            note(toml::Diagnostic::Warning, child(s, "position"),
                QStringLiteral("osd.position must be bottom-center, top-center or center"));
            c.osd.position = QStringLiteral("bottom-center");
        }
    }

    // ---- [quicksettings]
    if (const auto s = section("quicksettings")) {
        readBool(s, "enabled", c.quickSettings.enabled);
        readInt(s, "width", c.quickSettings.width);
        readInt(s, "destroy-grace", c.quickSettings.destroyGraceMs);
        readBool(s, "bluetooth", c.quickSettings.showBluetooth);
        readBool(s, "wifi-list", c.quickSettings.showWifiList);
        readBool(s, "power-profile", c.quickSettings.showPowerProfile);
        readBool(s, "night-light", c.quickSettings.showNightLight);
        readBool(s, "battery-details", c.quickSettings.showBatteryDetails);
        readBool(s, "scan-on-open", c.quickSettings.scanOnOpen);
        readInt(s, "volume-step", c.quickSettings.volumeStep);
        readInt(s, "brightness-step", c.quickSettings.brightnessStep);
        c.quickSettings.volumeStep = qBound(1, c.quickSettings.volumeStep, 25);
        c.quickSettings.brightnessStep = qBound(1, c.quickSettings.brightnessStep, 25);
    }

    // ---- [lock]
    if (const auto s = section("lock")) {
        readBool(s, "enabled", c.lock.enabled);
        readInt(s, "grace", c.lock.graceSeconds);
        readBool(s, "lock-on-suspend", c.lock.lockOnSuspend);
        readBool(s, "lock-on-lid-close", c.lock.lockOnLidClose);
        readInt(s, "wrong-password-delay", c.lock.wrongPasswordDelayMs);
        readInt(s, "retry-lockout", c.lock.retryLockoutMs);
        readInt(s, "retry-lockout-after", c.lock.retryLockoutAfter);
        readBool(s, "show-clock", c.lock.showClock);
        readBool(s, "show-keyboard-layout", c.lock.showKeyboardLayout);
        readString(s, "background", c.lock.background);
        c.lock.backgroundColor = parseColor(child(s, "background-color"), c.lock.backgroundColor,
            QStringLiteral("lock.background-color"), diagnostics);
        if (c.lock.background != QLatin1String("solid") && c.lock.background != QLatin1String("wallpaper")) {
            note(toml::Diagnostic::Warning, child(s, "background"),
                QStringLiteral("lock.background must be 'solid' or 'wallpaper'"));
            c.lock.background = QStringLiteral("solid");
        }
    }

    // ---- [idle]
    if (const auto s = section("idle")) {
        readInt(s, "dim-seconds", c.idle.dimSeconds);
        readInt(s, "dim-percent", c.idle.dimPercent);
        readInt(s, "lock-seconds", c.idle.lockSeconds);
        readInt(s, "dpms-off-seconds", c.idle.dpmsOffSeconds);
        readInt(s, "suspend-seconds", c.idle.suspendSeconds);
        readBool(s, "inhibit-while-playing", c.idle.inhibitWhilePlaying);
        readBool(s, "inhibit-on-fullscreen", c.idle.inhibitOnFullscreen);
        readBool(s, "inhibit-while-busy", c.idle.inhibitWhileBusy);
        readInt(s, "resume-grace", c.idle.resumeGraceSeconds);
        c.idle.dimPercent = qBound(10, c.idle.dimPercent, 100);
        c.idle.resumeGraceSeconds = qBound(0, c.idle.resumeGraceSeconds, 600);
    }

    // ---- [wallpaper]
    if (const auto s = section("wallpaper")) {
        readString(s, "mode", c.wallpaper.mode);
        readString(s, "path", c.wallpaper.path);
        readInt(s, "max-rss-kb-per-output", c.wallpaper.maxRssKbPerOutput);
        c.wallpaper.color = parseColor(child(s, "color"), c.wallpaper.color,
            QStringLiteral("wallpaper.color"), diagnostics);
        const QString mode = c.wallpaper.mode.toLower();
        if (mode != QLatin1String("solid") && mode != QLatin1String("none")
            && mode != QLatin1String("image")) {
            note(toml::Diagnostic::Warning, child(s, "mode"),
                QStringLiteral("wallpaper.mode must be solid, none or image"));
            c.wallpaper.mode = QStringLiteral("solid");
        }
        if (mode == QLatin1String("image") && c.wallpaper.path.isEmpty()) {
            note(toml::Diagnostic::Warning, nullptr,
                QStringLiteral("wallpaper.mode = \"image\" needs a path"));
            c.wallpaper.mode = QStringLiteral("solid");
        }
    }

    // ---- [tray]
    if (const auto s = section("tray")) {
        readBool(s, "enabled", c.tray.enabled);
        readInt(s, "icon-size", c.tray.iconSize);
        readInt(s, "spacing", c.tray.spacing);
        readInt(s, "max-items", c.tray.maxItems);
        readBool(s, "menu-on-left-click", c.tray.menuOnLeftClick);
        readBool(s, "context-menus", c.tray.contextMenus);
        c.tray.maxItems = qBound(0, c.tray.maxItems, 64);
        c.tray.iconSize = qBound(12, c.tray.iconSize, 64);
    }

    // ---- [mpris]
    if (const auto s = section("mpris")) {
        readBool(s, "enabled", c.mpris.enabled);
        readInt(s, "max-track-chars", c.mpris.maxTrackChars);
    }

    // ---- [bluetooth]
    if (const auto s = section("bluetooth")) {
        readBool(s, "enabled", c.bluetooth.enabled);
        readInt(s, "scan-timeout", c.bluetooth.scanTimeoutMs);
        readInt(s, "max-devices", c.bluetooth.maxDevices);
        c.bluetooth.maxDevices = qBound(1, c.bluetooth.maxDevices, 32);
    }

    // ---- [services]
    if (const auto s = section("services")) {
        if (const auto v = child(s, "network-backends")) {
            QVector<QString> backends;
            if (v->isString()) {
                backends = v->toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
            } else if (v->isArray()) {
                for (const auto &item : v->toArray()) {
                    backends.append(item->toString());
                }
            }
            for (QString &b : backends) {
                b = b.trimmed().toLower();
            }
            c.services.networkBackends = backends;
        }
        readString(s, "audio-backend", c.services.audioBackend);
        readBool(s, "upower", c.services.upower);
        readBool(s, "udev-battery", c.services.udevBattery);
        readBool(s, "nm-secret-agent", c.services.nmSecretAgent);
        readBool(s, "nm-wifi-list", c.services.nmWifiList);
        readBool(s, "perf-logging", c.services.perfLogging);
    }

    c.revision = previous.revision + 1;
    return c;
}

ConfigLoadResult loadConfig(const QString &path, const Config &previous)
{
    ConfigLoadResult result;
    result.config = previous;

    const QFileInfo info(path);
    if (!info.exists()) {
        // Section 7: no config file is a valid, fully working configuration.
        result.config = Config::defaults();
        result.usedDefaults = true;
        result.diagnostics.append({ toml::Diagnostic::Warning, 0, 0,
            QStringLiteral("no config at '%1'; using defaults").arg(path) });
        return result;
    }

    const toml::Document doc = toml::parseFile(path);
    result.diagnostics = doc.diagnostics;

    if (doc.hasErrors()) {
        // Keep the previous snapshot. This is the "a bad config must never
        // prevent startup" rule: we log precisely and carry on.
        result.rejected = true;
        result.config = previous;
        return result;
    }

    result.config = configFromToml(doc, path, previous, &result.diagnostics);
    return result;
}

QString defaultConfigToml()
{
    const Config d = Config::defaults();
    QString out;
    out += QStringLiteral("# %1 configuration (ADR-005).\n").arg(QLatin1String(KAPAH_NAME));
    out += QStringLiteral("# Every key below is shown with its default value. The shell is fully\n");
    out += QStringLiteral("# usable with no config file at all; this file only documents and\n");
    out += QStringLiteral("# overrides. Unknown keys warn and are ignored.\n\n");

    out += QStringLiteral("[general]\n");
    out += QStringLiteral("# scale = 1.0                # multiplier on all metrics\n");
    out += QStringLiteral("# font-family = \"\"           # empty = system font\n");
    out += QStringLiteral("theme = \"%1\"             # dark | light | system\n").arg(d.general.theme);
    out += QStringLiteral("animations = false           # off by default; see ADR-011\n");
    out += QStringLiteral("compact = false\n");
    out += QStringLiteral("reduce-motion = false\n");
    out += QStringLiteral("# locale = \"\"\n\n");

    out += QStringLiteral("[theme]\n");
    out += QStringLiteral("radius = 8\nradius-small = 6\nradius-large = 12\n");
    out += QStringLiteral("border-width = 1\nopacity = true\n");
    out += QStringLiteral("[theme.colors]\n");
    out += QStringLiteral("# bg = \"#101115\"\n# fg = \"#e6e6ea\"\n");
    out += QStringLiteral("# accent = \"#3d7eff\"\n# urgent = \"#ff5f56\"\n# muted = \"#8a8f98\"\n\n");

    out += QStringLiteral("[bar]\n");
    out += QStringLiteral("enabled = true\nposition = \"top\"      # top | bottom\n");
    out += QStringLiteral("visibility = \"always\"  # always | auto-hide | hidden\n");
    out += QStringLiteral("height = 28\nspacing = 4\npadding = 4\n");
    out += QStringLiteral("margins = [0, 4, 0, 4]  # [top, right, bottom, left]\n");
    out += QStringLiteral("auto-hide-on-fullscreen = false\n\n");
    out += QStringLiteral("[bar.left]\nmodules = [\"%1\"]\n\n").arg(
        QStringList { barModuleIdToString(BarModuleId::Workspaces) }.join(QLatin1Char(',')));
    out += QStringLiteral("[bar.center]\nmodules = [\"%1\"]\n\n").arg(
        barModuleIdToString(BarModuleId::WindowTitle));
    QStringList rightNames;
    for (BarModuleId id : d.bar.right.modules) {
        rightNames.append(barModuleIdToString(id));
    }
    out += QStringLiteral("[bar.right]\nmodules = [%1]\n\n").arg(
        rightNames.join(QStringLiteral("\", \"")));

    out += QStringLiteral("[notifications]\n");
    out += QStringLiteral("enabled = true\ntimeout = 5000\n");
    out += QStringLiteral("default-timeout = -2   # -2 = honour the app's value\n");
    out += QStringLiteral("max-visible = 4\nhistory-size = 50\n");
    out += QStringLiteral("group-bursts = true\nburst-window = 1500\n");
    out += QStringLiteral("per-app-rate-limit = 20\n");
    out += QStringLiteral("position = \"top-right\"\nmargin = 12\nwidth = 340\n");
    out += QStringLiteral("do-not-disturb = false\npanel = true\n\n");

    out += QStringLiteral("[launcher]\n");
    out += QStringLiteral("enabled = true\nwidth = 640\nmax-results = 9\n");
    out += QStringLiteral("icon-size = 32\ndestroy-grace = 30000\n");
    out += QStringLiteral("terminal = \"xterm\"\nlaunch-method = \"spawn\"\n");
    out += QStringLiteral("calculator = true\ncommand-runner = true\n");
    out += QStringLiteral("window-switcher = true\nclipboard = false\nemoji = false\n\n");

    out += QStringLiteral("[osd]\nenabled = true\ntimeout = 1500\n");
    out += QStringLiteral("position = \"bottom-center\"\nwidth = 220\nheight = 96\n");
    out += QStringLiteral("suppress-after-startup = 1500\n\n");

    out += QStringLiteral("[quicksettings]\nenabled = true\nwidth = 320\n");
    out += QStringLiteral("destroy-grace = 15000\nbluetooth = true\n");
    out += QStringLiteral("wifi-list = true\npower-profile = true\n");
    out += QStringLiteral("night-light = false\nvolume-step = 5\nbrightness-step = 5\n\n");

    out += QStringLiteral("[lock]\nenabled = true\ngrace = 0\n");
    out += QStringLiteral("lock-on-suspend = true\nlock-on-lid-close = true\n");
    out += QStringLiteral("wrong-password-delay = 1500\nretry-lockout = 30000\n");
    out += QStringLiteral("retry-lockout-after = 5\nshow-clock = true\n");
    out += QStringLiteral("background = \"solid\"\n\n");

    out += QStringLiteral("[idle]\ndim-seconds = 120\ndim-percent = 55\n");
    out += QStringLiteral("lock-seconds = 300\ndpms-off-seconds = 600\n");
    out += QStringLiteral("suspend-seconds = 0\ninhibit-while-playing = true\n");
    out += QStringLiteral("inhibit-on-fullscreen = true\nresume-grace = 30\n\n");

    out += QStringLiteral("[wallpaper]\nmode = \"solid\"      # solid | none | image\n");
    out += QStringLiteral("color = \"#101115\"\n# path = \"~/Pictures/wall.png\"\n\n");

    out += QStringLiteral("[tray]\nenabled = true\nicon-size = 18\nspacing = 2\n");
    out += QStringLiteral("max-items = 16\nmenu-on-left-click = false\n");
    out += QStringLiteral("context-menus = true\n\n");

    out += QStringLiteral("[mpris]\nenabled = false\n\n");
    out += QStringLiteral("[bluetooth]\nenabled = true\nmax-devices = 8\n\n");
    out += QStringLiteral("[services]\n");
    out += QStringLiteral("audio-backend = \"libpulse\"   # ADR-003\n");
    out += QStringLiteral("network-backends = [\"networkmanager\", \"netlink\"]\n");
    out += QStringLiteral("upower = true\nudev-battery = true\n");
    out += QStringLiteral("nm-secret-agent = false      # deferred, ADR-008\n");
    out += QStringLiteral("perf-logging = false\n");
    return out;
}

// ---------------------------------------------------------------------------
// BarConfig per-output resolution
// ---------------------------------------------------------------------------

bool BarConfig::enabledForOutput(const QString &outputName) const
{
    return forOutput(outputName).enabled;
}

BarConfig BarConfig::forOutput(const QString &outputName) const
{
    // Exact match wins, then the "*" wildcard, then the base config. The base
    // config is copied so callers can mutate freely.
    const auto exact = perOutput.constFind(outputName);
    if (exact != perOutput.constEnd()) {
        BarConfig sub = exact.value();
        sub.perOutput.clear();
        return sub;
    }
    const auto wildcard = perOutput.constFind(QStringLiteral("*"));
    if (wildcard != perOutput.constEnd()) {
        BarConfig sub = wildcard.value();
        sub.perOutput.clear();
        return sub;
    }
    BarConfig copy = *this;
    copy.perOutput.clear();
    return copy;
}

// ---------------------------------------------------------------------------
// ConfigWatcher
// ---------------------------------------------------------------------------

ConfigWatcher::ConfigWatcher(QObject *parent)
    : QObject(parent)
    , m_config(Config::defaults())
{
}

ConfigWatcher::~ConfigWatcher() = default;

void ConfigWatcher::start(const QString &configFilePath)
{
    if (m_path == configFilePath && m_watcher != nullptr) {
        return;
    }
    stop();

    m_path = configFilePath;

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    // Section 7: 100 ms debounce, and single-shot so no periodic wakeups.
    m_debounce->setInterval(100);
    m_debounce->setTimerType(Qt::PreciseTimer);
    connect(m_debounce, &QTimer::timeout, this, &ConfigWatcher::handleReload);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this,
        [this](const QString &) { scheduleReload(); });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
        [this](const QString &) { scheduleReload(); });

    // Watch the *directory*, not the file: editors and `mv` replace the inode,
    // and a watch on the old inode then never fires again.
    const QString dir = QFileInfo(m_path).absolutePath();
    if (!m_watcher->addPath(dir)) {
        LOG_WARNF(Config, "cannot watch config directory '%s'", qPrintable(dir));
    }
    // Also watch the file when it exists, so an in-place edit is caught even if
    // the directory watch is coalesced away.
    if (QFileInfo::exists(m_path)) {
        m_watcher->addPath(m_path);
    }

    // Initial load: read once, synchronously, before the first frame.
    ConfigLoadResult result = loadConfig(m_path, Config::defaults());
    for (const toml::Diagnostic &d : result.diagnostics) {
        LOG_WARNF(Config, "config: %s", qPrintable(d.format()));
    }
    m_config = result.config;

    LOG_INFOF(Config, "configuration loaded from %s (revision %d, %s)",
        qPrintable(m_config.sourceFile.isEmpty() ? QStringLiteral("<defaults>")
                                                 : m_config.sourceFile),
        m_config.revision, result.usedDefaults ? "defaults" : "file");
}

void ConfigWatcher::stop()
{
    if (m_watcher != nullptr) {
        delete m_watcher;
        m_watcher = nullptr;
    }
    if (m_debounce != nullptr) {
        delete m_debounce;
        m_debounce = nullptr;
    }
    m_path.clear();
}

void ConfigWatcher::scheduleReload()
{
    if (m_debounce != nullptr) {
        // start() restarts the timer, so a burst of writes collapses to one
        // read.
        m_debounce->start();
    }
}

bool ConfigWatcher::reloadNow()
{
    if (m_path.isEmpty()) {
        return false;
    }
    handleReload();
    return true;
}

void ConfigWatcher::handleReload()
{
    // Re-add watches: the rename that triggered this may have removed them.
    if (m_watcher != nullptr) {
        const QString dir = QFileInfo(m_path).absolutePath();
        if (!m_watcher->directories().contains(dir)) {
            m_watcher->addPath(dir);
        }
        if (!m_watcher->files().contains(m_path) && QFileInfo::exists(m_path)) {
            m_watcher->addPath(m_path);
        }
    }

    applyResult(loadConfig(m_path, m_config));
}

void ConfigWatcher::applyResult(ConfigLoadResult result)
{
    for (const toml::Diagnostic &d : result.diagnostics) {
        if (result.usedDefaults && d.severity == toml::Diagnostic::Warning && d.line == 0) {
            continue; // "no config file" is normal, not worth repeating
        }
        LOG_WARNF(Config, "config: %s", qPrintable(d.format()));
    }

    if (result.rejected) {
        LOG_WARN(Config,
            "configuration has errors; keeping the previous revision (the shell is unaffected)");
        return;
    }

    const Config old = m_config;
    if (old.generalEquals(result.config) && old.themeEquals(result.config)
        && old.barEquals(result.config) && old.notificationsEqual(result.config)
        && old.launcherEquals(result.config) && old.osdEquals(result.config)
        && old.quickSettingsEqual(result.config) && old.lockEquals(result.config)
        && old.idleEquals(result.config) && old.wallpaperEquals(result.config)
        && old.trayEquals(result.config) && old.mprisEquals(result.config)
        && old.bluetoothEquals(result.config) && old.servicesEqual(result.config)) {
        LOG_CAT(Config, "configuration re-read; nothing changed");
        return;
    }

    m_config = result.config;
    LOG_INFOF(Config, "configuration reloaded (revision %d)", m_config.revision);

    Q_EMIT reloaded();

    // Fire only the sections that actually differ. This is the mechanism behind
    // "diff old vs new, update only affected components" (section 7).
    if (!old.generalEquals(m_config) && m_generalCb) {
        m_generalCb();
    }
    if (!old.themeEquals(m_config) && m_themeCb) {
        m_themeCb();
    }
    if (!old.barEquals(m_config) && m_barCb) {
        m_barCb();
    }
    if (!old.notificationsEqual(m_config) && m_notificationsCb) {
        m_notificationsCb();
    }
    if (!old.launcherEquals(m_config) && m_launcherCb) {
        m_launcherCb();
    }
    if (!old.osdEquals(m_config) && m_osdCb) {
        m_osdCb();
    }
    if (!old.quickSettingsEqual(m_config) && m_quickSettingsCb) {
        m_quickSettingsCb();
    }
    if (!old.lockEquals(m_config) && m_lockCb) {
        m_lockCb();
    }
    if (!old.idleEquals(m_config) && m_idleCb) {
        m_idleCb();
    }
    if (!old.wallpaperEquals(m_config) && m_wallpaperCb) {
        m_wallpaperCb();
    }
    if (!old.trayEquals(m_config) && m_trayCb) {
        m_trayCb();
    }
    if (!old.mprisEquals(m_config) && m_mprisCb) {
        m_mprisCb();
    }
    if (!old.bluetoothEquals(m_config) && m_bluetoothCb) {
        m_bluetoothCb();
    }
    if (!old.servicesEqual(m_config) && m_servicesCb) {
        m_servicesCb();
    }
}

void ConfigWatcher::onGeneralChanged(SectionCallback cb) { m_generalCb = std::move(cb); }
void ConfigWatcher::onThemeChanged(SectionCallback cb) { m_themeCb = std::move(cb); }
void ConfigWatcher::onBarChanged(SectionCallback cb) { m_barCb = std::move(cb); }
void ConfigWatcher::onNotificationsChanged(SectionCallback cb)
{
    m_notificationsCb = std::move(cb);
}
void ConfigWatcher::onLauncherChanged(SectionCallback cb) { m_launcherCb = std::move(cb); }
void ConfigWatcher::onOsdChanged(SectionCallback cb) { m_osdCb = std::move(cb); }
void ConfigWatcher::onQuickSettingsChanged(SectionCallback cb)
{
    m_quickSettingsCb = std::move(cb);
}
void ConfigWatcher::onLockChanged(SectionCallback cb) { m_lockCb = std::move(cb); }
void ConfigWatcher::onIdleChanged(SectionCallback cb) { m_idleCb = std::move(cb); }
void ConfigWatcher::onWallpaperChanged(SectionCallback cb) { m_wallpaperCb = std::move(cb); }
void ConfigWatcher::onTrayChanged(SectionCallback cb) { m_trayCb = std::move(cb); }
void ConfigWatcher::onMprisChanged(SectionCallback cb) { m_mprisCb = std::move(cb); }
void ConfigWatcher::onBluetoothChanged(SectionCallback cb) { m_bluetoothCb = std::move(cb); }
void ConfigWatcher::onServicesChanged(SectionCallback cb) { m_servicesCb = std::move(cb); }

} // namespace kapah