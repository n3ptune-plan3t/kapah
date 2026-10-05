// Category-based logger.
//
// Design constraints from the roadmap:
//  * Section 0.2: idle shell must use ~0% CPU. Logging must never allocate at
//    idle, never touch the filesystem, and never flush on its own.
//  * Section 4.1: no polling. So no timestamp thread and no size-based rotation;
//    log to stderr only (the journal, if any, is systemd's problem).
//  * Phase 0.4: "tiny category-based logger, compile-time disable of debug in
//    release".
//
// Therefore:
//  * The category mask is a compile-time constant in release builds, which lets
//    the compiler fold disabled categories into nothing at all.
//  * Messages go to stderr unbuffered (libc stderr is unbuffered by default and
//    we bypass Qt's message handler), so there is nothing to flush at exit.

#pragma once

#include <QString>
#include <QtGlobal>

#include <cstdarg>
#include <cstdio>

#include "kapah/version.h"

namespace kapah::log {

// Compile-time categories. Keep the list short: every category is a bit.
enum Category : quint32 {
    None = 0,
    Core = 1u << 0,
    Config = 1u << 1,
    Wayland = 1u << 2,
    Niri = 1u << 3,
    Power = 1u << 4,
    Network = 1u << 5,
    Audio = 1u << 6,
    Backlight = 1u << 7,
    Tray = 1u << 8,
    Mpris = 1u << 9,
    Bluetooth = 1u << 10,
    Notify = 1u << 11,
    Launcher = 1u << 12,
    Ipc = 1u << 13,
    Ui = 1u << 14,
    Lock = 1u << 15,
    Perf = 1u << 16,
    All = (1u << 17) - 1,
};

// Default masks. KAPAH_DEBUG_BUILD is 0 for Release and MinSizeRel.
#if KAPAH_DEBUG_BUILD
inline constexpr quint32 kDebugMask = All;
inline constexpr quint32 kInfoMask = All;
#else
inline constexpr quint32 kDebugMask = None;
inline constexpr quint32 kInfoMask = Core | Config | Niri | Ipc | Perf | Lock | Ui;
#endif

// Warnings are the only thing that survives a stripped release build, so they
// are always compiled in: they are how a misbehaving daemon stays debuggable.
inline constexpr quint32 kWarnMask = Core | Config | Wayland | Niri | Power | Network | Audio
    | Backlight | Tray | Mpris | Bluetooth | Notify | Launcher | Ipc | Ui | Lock | Perf;

struct State {
    // ANDed with the compile-time masks at every call site.
    quint32 mask = kWarnMask;
    bool timestamps = false;
    // Optional file mirror. Off by default: an idle shell should not hold a file
    // descriptor it does not need. Only used by the soak tests and `kapahctl`.
    std::FILE *fileSink = nullptr;
};

// One-time process init; safe to call more than once.
void init();
// Restrict to specific categories, e.g. "niri,ui". Empty string = defaults.
// Unknown names are reported and ignored so a typo in a systemd unit cannot
// silence the whole log.
bool setCategories(const QString &commaSeparated);
void setTimestamps(bool enabled);
bool openSink(const QString &path);
void closeSink();

inline bool enabled(Category c)
{
    return (State::mask & static_cast<quint32>(c)) != 0;
}

void emit(Category cat, const char *file, int line, const QString &message);

QString format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void emitFormatted(Category cat, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

inline const char *shortFile(const char *path)
{
    const char *slash = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/') {
            slash = p + 1;
        }
    }
    return slash;
}

} // namespace kapah::log

// All four families collapse into one shape:
//
//   if constexpr (compile-time mask allows this category) {
//       if (runtime mask allows this category) { emit(...) }
//   }
//
// so a disabled category costs one `if constexpr` that emits no code at all.

#define KAPAH_LOG_(cat, level, stmt)                                                  \
    do {                                                                              \
        constexpr ::kapah::log::Category kCat = (cat);                                 \
        if constexpr ((::kapah::log::level & static_cast<::kapah::quint32>(cat)) != 0) { \
            if (::kapah::log::enabled(kCat)) {                                        \
                stmt;                                                                 \
            }                                                                         \
        }                                                                              \
    } while (false)

#define LOG_CAT(cat, msg) KAPAH_LOG_(cat, kDebugMask, ::kapah::log::emit(kCat, __FILE__, __LINE__, QStringLiteral(msg)))
#define LOG_INFO(cat, msg) KAPAH_LOG_(cat, kInfoMask, ::kapah::log::emit(kCat, __FILE__, __LINE__, QStringLiteral(msg)))
#define LOG_WARN(cat, msg) KAPAH_LOG_(cat, kWarnMask, ::kapah::log::emit(kCat, __FILE__, __LINE__, QStringLiteral(msg)))
#define LOG_ERR(cat, msg) LOG_WARN(cat, msg)

#define LOG_CATF(cat, fmt, ...) KAPAH_LOG_(cat, kDebugMask, ::kapah::log::emitFormatted(kCat, __FILE__, __LINE__, fmt, ##__VA_ARGS__))
#define LOG_INFOF(cat, fmt, ...) KAPAH_LOG_(cat, kInfoMask, ::kapah::log::emitFormatted(kCat, __FILE__, __LINE__, fmt, ##__VA_ARGS__))
#define LOG_WARNF(cat, fmt, ...) KAPAH_LOG_(cat, kWarnMask, ::kapah::log::emitFormatted(kCat, __FILE__, __LINE__, fmt, ##__VA_ARGS__))

// Perf numbers are logged in release builds: tools/perf parses these lines.
#define LOG_PERF(...) LOG_WARNF(Perf, __VA_ARGS__)