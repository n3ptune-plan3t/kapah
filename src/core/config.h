// Configuration schema for kapah (ADR-005).
//
// Every field has a default that produces a fully usable shell with no config
// file at all (section 7). Parsing never fails the shell: on error the last
// known-good in-memory config stays in force.
//
// Hot reload watches the *parent directory* of the config file, not the file
// itself, because editors replace files atomically and a watch on the inode
// goes dead silently.

#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QMargins>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <memory>

#include <kapah/toml.h>

namespace kapah {

enum class BarPosition { Top, Bottom };
enum class BarVisibility { Always, AutoHide, Hidden };
enum class BarModuleId {
    Workspaces,
    WindowTitle,
    Clock,
    Battery,
    Network,
    Audio,
    Backlight,
    Tray,
    Mpris,
    Layout,
    Session,
};

QString barModuleIdToString(BarModuleId id);
bool barModuleIdFromString(const QString &s, BarModuleId *out);
QStringList allBarModuleIds();

// ---------------------------------------------------------------------------
// Sub-sections
// ---------------------------------------------------------------------------

struct GeneralConfig {
    // Multiplier on every metric in the theme. 1.0 = design size.
    double scale = 1.0;
    // Empty = system default via QFontDatabase::systemFont().
    QString fontFamily;
    // "" = follow the Qt palette / GTK theme; "light" / "dark" force it.
    QString theme = QStringLiteral("dark");
    bool animations = false;
    // When true, the clock label hides the seconds and the date unless asked.
    bool compact = false;
    // Reduce paint work: disables all hover highlighting transitions.
    bool reduceMotion = false;
    QString locale;
};

struct BarSectionConfig {
    QVector<BarModuleId> modules;

    static BarSectionConfig defaults();
};

struct BarConfig {
    bool enabled = true;
    BarPosition position = BarPosition::Top;
    BarVisibility visibility = BarVisibility::Always;
    int height = 28;
    QMargins margins = QMargins(4, 0, 4, 0);
    int spacing = 4;
    int padding = 4;
    // Hide when a window is fullscreen on the same output.
    bool autoHideOnFullscreen = false;
    // Per-output overrides keyed by output name; "*" applies to all.
    QHash<QString, BarConfig> perOutput;
    BarSectionConfig left = BarSectionConfig::defaults();
    BarSectionConfig center;
    BarSectionConfig right;

    bool enabledForOutput(const QString &outputName) const;
    BarConfig forOutput(const QString &outputName) const;
};

struct NotificationConfig {
    bool daemonEnabled = true;
    int timeoutMs = 5000;
    // 0 = use timeoutMs; -1 = never expire (unless the app says so).
    int defaultTimeoutOverride = -2;
    int maxVisible = 4;
    int historySize = 50;
    int historyBodyMaxChars = 240;
    int historyImageMaxPx = 64;
    // Collapse a burst from the same app+summary into one notification.
    bool groupBursts = true;
    int burstWindowMs = 1500;
    // Ignore more than this many notifications per app per minute.
    int perAppRateLimit = 20;
    QString position = QStringLiteral("top-right");
    int margin = 12;
    int width = 340;
    bool doNotDisturb = false;
    // app-id (or desktop-entry) -> "mute" | "persist"
    QHash<QString, QString> perAppRules;
    bool persistHistoryOnExit = true;
    bool panelEnabled = true;
    int panelWidth = 360;
    int panelHeight = 460;
};

struct LauncherConfig {
    bool enabled = true;
    int width = 640;
    int maxResults = 9;
    int iconSize = 32;
    // Destroy the window this many ms after it is hidden (4.3 lazy construction).
    int destroyGraceMs = 30000;
    QString terminal = QStringLiteral("xterm");
    // "spawn" = posix_spawn directly, "systemd" = systemd-run --user --scope.
    QString launchMethod = QStringLiteral("spawn");
    // Wait this long before an idle trim after closing.
    int trimDelayMs = 2000;
    bool showKeywords = false;
    bool showGenericName = false;
    // Per-mode enable flags.
    bool calculatorEnabled = true;
    bool commandRunnerEnabled = true;
    bool windowSwitcherEnabled = true;
    bool clipboardEnabled = false;
    bool emojiEnabled = false;
    // Max entries kept in the frecency file.
    int frecencySize = 256;
};

struct OsdConfig {
    bool enabled = true;
    int timeoutMs = 1500;
    // Where on screen: "bottom-center", "center", "top-center".
    QString position = QStringLiteral("bottom-center");
    int width = 220;
    int height = 96;
    // Suppress the OSD for the first N ms after daemon start, so restoring a
    // saved volume does not fire a popup (Phase 6.2).
    int suppressAfterStartupMs = 1500;
    bool showOnKeypress = true;
    bool showBrightness = true;
};

struct QuickSettingsConfig {
    bool enabled = true;
    int width = 320;
    int destroyGraceMs = 15000;
    bool showBluetooth = true;
    bool showWifiList = true;
    bool showPowerProfile = true;
    bool showNightLight = false;
    bool showBatteryDetails = true;
    // Devices are only enumerated while the panel is open (Phase 7.4).
    bool scanOnOpen = false;
    int volumeStep = 5;
    int brightnessStep = 5;
};

struct LockConfig {
    bool enabled = true;
    int graceSeconds = 0;
    int lockOnSuspend = true;
    int lockOnLidClose = true;
    int wrongPasswordDelayMs = 1500;
    int retryLockoutMs = 30000;
    int retryLockoutAfter = 5;
    bool showClock = true;
    bool showKeyboardLayout = true;
    // Solid colour behind the wallpaper, or the wallpaper itself.
    QString background = QStringLiteral("solid");
    QColor backgroundColor = QColor(16, 17, 21);
};

struct IdleConfig {
    // Seconds of idleness before each step. 0 or negative disables the step.
    int dimSeconds = 120;
    int dimPercent = 55;
    int lockSeconds = 300;
    int dpmsOffSeconds = 600;
    int suspendSeconds = 0;
    // Honour MPRIS "playing" as an inhibitor.
    bool inhibitWhilePlaying = true;
    bool inhibitOnFullscreen = true;
    bool inhibitWhileBusy = true;
    // Re-arm delay when an inhibitor is released.
    int resumeGraceSeconds = 30;
};

struct WallpaperConfig {
    // "solid", "none" (let the compositor draw its own), or a path/glob.
    QString mode = QStringLiteral("solid");
    QString path;
    QColor color = QColor(16, 17, 21);
    // Roadmap 9.1: refuse to enable if per-output RSS exceeds this.
    int maxRssKbPerOutput = 4096;
};

struct ThemeColorsConfig {
    // Empty = built-in palette for the chosen light/dark variant.
    QHash<QString, QString> overrides;
    int radius = 8;
    int radiusSmall = 6;
    int radiusLarge = 12;
    int borderWidth = 1;
    bool opacity = true;
};

struct TrayConfig {
    bool enabled = true;
    int iconSize = 18;
    int spacing = 2;
    // Hard cap: a misbehaving tray host must not make the bar 500 px wide.
    int maxItems = 16;
    bool menuOnLeftClick = false;
    bool contextMenus = true;
};

struct MprisConfig {
    bool enabled = false;
    int maxTrackChars = 60;
};

struct BluetoothConfig {
    bool enabled = true;
    int scanTimeoutMs = 6000;
    int maxDevices = 8;
};

struct ServiceConfig {
    // Which backends to try, in order. Empty = autodetect.
    QStringList networkBackends;
    QString audioBackend = QStringLiteral("libpulse");
    bool upower = true;
    bool udevBattery = true;
    bool nmSecretAgent = false; // see ADR-008 (deferred)
    bool nmWifiList = true;
    // Emission of LOG_PERF lines consumed by tools/perf.
    bool perfLogging = false;
};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// A full snapshot. Cheap to copy (a few hundred bytes of POD plus small QString
// refcounts) which is what makes the diff in ConfigWatcher trivial.
struct Config {
    int revision = 0;
    GeneralConfig general;
    ThemeColorsConfig theme;
    BarConfig bar;
    NotificationConfig notifications;
    LauncherConfig launcher;
    OsdConfig osd;
    QuickSettingsConfig quickSettings;
    LockConfig lock;
    IdleConfig idle;
    WallpaperConfig wallpaper;
    TrayConfig tray;
    MprisConfig mpris;
    BluetoothConfig bluetooth;
    ServiceConfig services;
    // Where this snapshot came from. Empty when defaults are in force.
    QString sourceFile;

    static Config defaults();

    // Section-by-section identity test used by ConfigWatcher to decide which
    // components need to react. Cheap: compares the POD fields and the QString
    // fields, not deep contents.
    bool generalEquals(const Config &o) const;
    bool themeEquals(const Config &o) const;
    bool barEquals(const Config &o) const;
    bool notificationsEqual(const Config &o) const;
    bool launcherEquals(const Config &o) const;
    bool osdEquals(const Config &o) const;
    bool quickSettingsEqual(const Config &o) const;
    bool lockEquals(const Config &o) const;
    bool idleEquals(const Config &o) const;
    bool wallpaperEquals(const Config &o) const;
    bool trayEquals(const Config &o) const;
    bool mprisEquals(const Config &o) const;
    bool bluetoothEquals(const Config &o) const;
    bool servicesEqual(const Config &o) const;
};

// Result of reading a config file.
struct ConfigLoadResult {
    Config config;
    QVector<toml::Diagnostic> diagnostics;
    bool usedDefaults = false;
    // True when the file was syntactically broken and we kept the previous
    // snapshot instead of applying the partial one.
    bool rejected = false;
};

ConfigLoadResult loadConfig(const QString &path, const Config &previous);

// Parses an in-memory TOML document. Exposed for tests.
Config configFromToml(const toml::Document &doc, const QString &sourceFile, const Config &previous,
    QVector<toml::Diagnostic> *diagnostics);

// Renders the defaults as a commented TOML file. Used by `kapahctl config
// --write-defaults` and by the docs.
QString defaultConfigToml();

// ---------------------------------------------------------------------------
// ConfigWatcher
// ---------------------------------------------------------------------------

// Owns the QFileSystemWatcher on the config *directory*, debounces 100 ms
// (section 7), re-reads, and reports which sections changed. When the file is
// broken it emits nothing and logs: the last good snapshot stays in force.
class ConfigWatcher : public QObject {
    Q_OBJECT
public:
    using SectionCallback = std::function<void()>;

    explicit ConfigWatcher(QObject *parent = nullptr);
    ~ConfigWatcher() override;

    const Config &config() const { return m_config; }

    // Starts watching. Safe to call twice.
    void start(const QString &configFilePath);
    void stop();

    // Re-reads immediately, bypassing the debounce. Used by `kapahctl reload`.
    bool reloadNow();

    // Registers an interest in a section. Called by the components themselves.
    // The watcher stores one callback per section and fires only what changed,
    // which is what "never rebuild everything" (section 7) means in code.
    void onGeneralChanged(SectionCallback cb);
    void onThemeChanged(SectionCallback cb);
    void onBarChanged(SectionCallback cb);
    void onNotificationsChanged(SectionCallback cb);
    void onLauncherChanged(SectionCallback cb);
    void onOsdChanged(SectionCallback cb);
    void onQuickSettingsChanged(SectionCallback cb);
    void onLockChanged(SectionCallback cb);
    void onIdleChanged(SectionCallback cb);
    void onWallpaperChanged(SectionCallback cb);
    void onTrayChanged(SectionCallback cb);
    void onMprisChanged(SectionCallback cb);
    void onBluetoothChanged(SectionCallback cb);
    void onServicesChanged(SectionCallback cb);

Q_SIGNALS:
    // Emitted after any successful reload, before the section callbacks.
    void reloaded();

private:
    void scheduleReload();
    void handleReload();
    void applyResult(ConfigLoadResult result);

    QString m_path;
    Config m_config = Config::defaults();
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounce = nullptr;

    SectionCallback m_generalCb;
    SectionCallback m_themeCb;
    SectionCallback m_barCb;
    SectionCallback m_notificationsCb;
    SectionCallback m_launcherCb;
    SectionCallback m_osdCb;
    SectionCallback m_quickSettingsCb;
    SectionCallback m_lockCb;
    SectionCallback m_idleCb;
    SectionCallback m_wallpaperCb;
    SectionCallback m_trayCb;
    SectionCallback m_mprisCb;
    SectionCallback m_bluetoothCb;
    SectionCallback m_servicesCb;
};

} // namespace kapah