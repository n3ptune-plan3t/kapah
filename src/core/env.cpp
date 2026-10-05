#include "env.h"

#include "logging.h"

#include <malloc.h>
#include <sched.h>

namespace kapah {

namespace {

void setIfUnset(const char *name, const char *value, QStringList &changed)
{
    if (qEnvironmentVariableIsEmpty(name)) {
        qputenv(name, value);
        changed.append(QString::fromLatin1(name));
    }
}

} // namespace

QStringList hardenEnvironment()
{
    QStringList changed;

    // Never override a platform the caller deliberately chose (offscreen for
    // headless tests, minimal for pure-core tools).
    setIfUnset("QT_QPA_PLATFORM", "wayland", changed);

    setIfUnset("QT_QUICK_BACKEND", "software", changed);
    setIfUnset("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", changed);
    setIfUnset("LIBGL_ALWAYS_SOFTWARE", "1", changed);
    setIfUnset("GALLIUM_DRIVER", "llvmpipe", changed);
    setIfUnset("QT_ENABLE_HIGHDPI_SCALING", "0", changed);
    setIfUnset("QSG_RENDER_LOOP", "basic", changed);

    // Qt's own knobs we rely on:
    //  - no icon theme cache thrash: we own the icon cache (src/ui/icons.cpp)
    //  - don't let the platform theme plugin try to load GTK/Qt styles: it is a
    //    pure QPainter shell and QStyle costs ~1 MB plus a plugin dlopen
    if (qEnvironmentVariableIsEmpty("QT_STYLE_OVERRIDE")) {
        qputenv("QT_STYLE_OVERRIDE", "Fusion");
        changed.append(QStringLiteral("QT_STYLE_OVERRIDE"));
    }

    // XDG: never let Qt read a system-wide icon theme we do not use.
    setIfUnset("QT_NO_GLIB", "1", changed);

    return changed;
}

void tuneAllocator()
{
#if defined(__GLIBC__)
    // Single-threaded hot paths (roadmap 4.1): one arena avoids per-thread
    // 64 MB reservations that never get returned to the OS.
    mallopt(M_ARENA_MAX, 1);
    // Trim aggressively; the default (128 kB) leaves a lot of freed icon and
    // JSON buffers resident after the launcher closes.
    mallopt(M_TRIM_THRESHOLD, 64 * 1024);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
}

void raiseSchedulingPriority()
{
    // SCHED_BATCH keeps us out of the compositor's wakeup critical path when we
    // do need to run. It never requires privileges.
    sched_param param {};
    param.sched_priority = 0;
    sched_setscheduler(0, SCHED_BATCH, &param);
}

void releaseFreedMemory()
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

} // namespace kapah