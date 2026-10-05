// Bar module contract, per HANDOVER section 6 step 2.
//
// A module is a QWidget living inside a BarItem's horizontal layout. It owns
// its own subscriptions: when the bar is rebuilt (config changed, output
// removed) the module widget is deleted, which tears the subscriptions down
// via the usual QObject parent mechanism. No module spins a timer; the only
// QTimers allowed are the clock tick, notification expiry, and debounce.
//
// Construction context: a module never looks for services globally. It is
// handed the full BarContext and uses what it needs. The bar is the only
// code that knows every service; the modules only know their one or two.

#pragma once

#include <QVector>
#include <QWidget>

#include "config.h"

namespace kapah {

class AudioService;
class BacklightService;
class IconCache;
class LogindService;
class NetworkService;
class PowerService;
class Theme;
class TrayService;

namespace niri {
class ActionSender;
class NiriState;
}

struct BarContext {
    Theme *theme = nullptr;
    IconCache *icons = nullptr;
    niri::NiriState *state = nullptr;
    niri::ActionSender *actions = nullptr;
    PowerService *power = nullptr;
    NetworkService *network = nullptr;
    AudioService *audio = nullptr;
    BacklightService *backlight = nullptr;
    TrayService *tray = nullptr;
    LogindService *logind = nullptr;
};

// Interface every module implements. `configure` is called on config change;
// `invalidate` tells the module its cached view of a service went stale (the
// service recovered from a daemon restart with different state).
class BarModule : public QWidget {
    Q_OBJECT
public:
    explicit BarModule(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);
    ~BarModule() override = default;

    virtual void configure() {}
    virtual void invalidate() {}

protected:
    const BarContext &ctx() const { return m_ctx; }
    QScreen *output() { return m_output; }

    BarContext m_ctx;
    QScreen *m_output = nullptr;
};

// Maps the config enum to a constructor. Modules that have no service yet
// (mpris) are absent from the map until their service exists; the bar then
// renders them as a gap rather than a crash.
BarModule *makeModule(BarModuleId id, const BarContext &ctx, QScreen *output,
    QWidget *parent);

} // namespace kapah
