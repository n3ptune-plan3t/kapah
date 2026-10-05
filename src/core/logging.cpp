#include "logging.h"

#include <QByteArray>
#include <QStringList>

#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace kapah::log {

namespace {

struct NameEntry {
    const char *name;
    Category cat;
};

constexpr NameEntry kNames[] = {
    {"core", Core},
    {"config", Config},
    {"wl", Wayland},
    {"wayland", Wayland},
    {"niri", Niri},
    {"power", Power},
    {"network", Network},
    {"audio", Audio},
    {"backlight", Backlight},
    {"tray", Tray},
    {"mpris", Mpris},
    {"bluetooth", Bluetooth},
    {"notify", Notify},
    {"launcher", Launcher},
    {"ipc", Ipc},
    {"ui", Ui},
    {"lock", Lock},
    {"perf", Perf},
};

constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

// Compact integer-to-text without touching QTextStream. Called once per emitted
// line, so it must not allocate.
void writeUnsigned(std::FILE *out, quint64 v)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v != 0) {
        buf[--i] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    std::fputs(buf + i, out);
}

const char *levelName(Category c)
{
    switch (c) {
    case Core:
    case Config:
    case Wayland:
    case Niri:
    case Power:
    case Network:
    case Audio:
    case Backlight:
    case Tray:
    case Mpris:
    case Bluetooth:
    case Notify:
    case Launcher:
    case Ipc:
    case Ui:
    case Lock:
    case Perf:
        return "I";
    default:
        return "?";
    }
}

} // namespace

void init()
{
    // Never touch the filesystem at startup: if KAPAH_LOG_FILE is set, the
    // caller (soak tests) opens the sink explicitly via kapahctl/--log-file.
}

bool setCategories(const QString &commaSeparated)
{
    if (commaSeparated.isEmpty()) {
        State::mask = kWarnMask;
        return true;
    }

    quint32 mask = 0;
    bool ok = true;

    const auto parts = commaSeparated.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString name = raw.trimmed().toLower();
        if (name == QLatin1String("all")) {
            mask |= kWarnMask;
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < kNameCount; ++i) {
            if (name == QLatin1String(kNames[i].name)) {
                mask |= static_cast<quint32>(kNames[i].cat);
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "kapah: unknown log category '%s'\n", qPrintable(name));
            ok = false;
        }
    }

    State::mask = mask;
    return ok;
}

void setTimestamps(bool enabledFlag)
{
    State::timestamps = enabledFlag;
}

bool openSink(const QString &path)
{
    closeSink();
    State::fileSink = std::fopen(qPrintable(path), "ae");
    return State::fileSink != nullptr;
}

void closeSink()
{
    if (State::fileSink != nullptr) {
        std::fclose(State::fileSink);
        State::fileSink = nullptr;
    }
}

void emit(Category cat, const char *file, int line, const QString &message)
{
    const QByteArray utf8 = message.toUtf8();

    if (State::timestamps) {
        struct timespec ts {};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm {};
        localtime_r(&ts.tv_sec, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        std::fprintf(stderr, "%s.%03ld ", stamp, ts.tv_nsec / 1000000L);
    }

    std::fprintf(stderr, "kapah[%s] %s:%d: %s\n", levelName(cat), shortFile(file), line,
        utf8.constData());
    if (State::fileSink != nullptr) {
        std::fprintf(State::fileSink, "%s:%d: %s\n", shortFile(file), line, utf8.constData());
    }
}

QString format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    QString out = QString::vasprintf(fmt, ap);
    va_end(ap);
    return out;
}

void emitFormatted(Category cat, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const QString msg = QString::vasprintf(fmt, ap);
    va_end(ap);
    emit(cat, file, line, msg);
}

} // namespace kapah::log