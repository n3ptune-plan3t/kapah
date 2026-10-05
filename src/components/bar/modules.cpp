#include "modules.h"

#include "audioitem.h"
#include "backlightitem.h"
#include "batteryitem.h"
#include "clockitem.h"
#include "layoutitem.h"
#include "networkitem.h"
#include "sessionitem.h"
#include "trayitem.h"
#include "windowtitleitem.h"
#include "workspaceitem.h"

namespace kapah {

BarModule::BarModule(const BarContext &ctx, QScreen *output, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_output(output)
{
}

BarModule *makeModule(BarModuleId id, const BarContext &ctx, QScreen *output,
    QWidget *parent)
{
    switch (id) {
    case BarModuleId::Workspaces:
        return new WorkspaceItem(ctx, output, parent);
    case BarModuleId::WindowTitle:
        return new WindowTitleItem(ctx, output, parent);
    case BarModuleId::Clock:
        return new ClockItem(ctx, output, parent);
    case BarModuleId::Battery:
        return new BatteryItem(ctx, output, parent);
    case BarModuleId::Network:
        return new NetworkItem(ctx, output, parent);
    case BarModuleId::Audio:
        return new AudioItem(ctx, output, parent);
    case BarModuleId::Backlight:
        return new BacklightItem(ctx, output, parent);
    case BarModuleId::Tray:
        return new TrayItemView(ctx, output, parent);
    case BarModuleId::Layout:
        return new LayoutItem(ctx, output, parent);
    case BarModuleId::Session:
        return new SessionItem(ctx, output, parent);
    case BarModuleId::Mpris:
        // MPRIS service does not exist yet (KAPAH_WITH_MPRIS is OFF); the bar
        // renders a gap for it rather than crashing.
        return nullptr;
    }
    return nullptr;
}

} // namespace kapah
