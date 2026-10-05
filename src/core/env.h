// Process-level hardening applied before any QCoreApplication exists.
//
// This is the single place where the software-rendering environment from
// roadmap section 1 is enforced, so no entry point can forget it.

#pragma once

#include <QStringList>

namespace kapah {

// Forces:
//   QT_QPA_PLATFORM=wayland              (only if unset; a headless test can
//                                          legitimately set "offscreen")
//   QT_QUICK_BACKEND=software            (belt and braces; nothing should link
//                                          Quick, see ADR-002)
//   QT_WAYLAND_DISABLE_WINDOWDECORATION=1
//   LIBGL_ALWAYS_SOFTWARE=1
//   GALLIUM_DRIVER=llvmpipe
//   QT_ENABLE_HIGHDPI_SCALING=0          (Qt6 always scales; explicit anyway)
//   QSG_RENDER_LOOP=basic                 (never reached, but harmless)
//
// Must be called before the QCoreApplication/QGuiApplication constructor.
// Returns the list of variables that were set, for --verbose startup logging.
QStringList hardenEnvironment();

// Reduces allocator overhead: a single-threaded process with one arena saves a
// few hundred kB of PSS. No-op on non-glibc.
void tuneAllocator();

// Drops the scheduler's "prefer not to be migrated / give up nice" bias, which
// lets the compositor idle longer. Optional, ~1 ms at startup.
void raiseSchedulingPriority();

// Best-effort malloc_trim(0). Called on an idle single-shot after the launcher
// closes (roadmap 4.3). No-op if glibc is not in use.
void releaseFreedMemory();

} // namespace kapah