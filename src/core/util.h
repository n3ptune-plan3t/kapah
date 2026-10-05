// Small helpers shared across the shell. Deliberately tiny: anything that grows
// past a few dozen lines belongs in its own translation unit.

#pragma once

#include <QByteArray>
#include <QMargins>
#include <QRect>
#include <QString>
#include <QStringView>

#include <cstddef>

namespace kapah::util {

// Case-insensitive, accent-naive fold for search. Lowercases ASCII in place
// without allocating a second string when the input is already lower.
QString fold(const QString &s);
QByteArray foldToUtf8(const QString &s);

// True when `haystack` contains every character of `needle` in order
// (subsequence match). Used by the launcher's cheap pre-filter.
bool isSubsequence(QStringView haystackLower, QStringView needleLower);

// True when `needle` is a prefix of `haystack`.
bool startsWithFold(QStringView haystack, QStringView needle);

// Elides `text` to `width` px with the given metrics, appending an ellipsis.
QString elide(const QString &text, int width, const class QFontMetrics &fm);

// Human-readable byte count for logs: "1.4 MB".
QString formatBytes(qint64 bytes);

// Clamps a value to a range.
template<typename T>
constexpr T clamp(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Deviates from every surface for a few ms, at most once. Used by tests.
bool sleepMs(int ms);

// The user's login session object path for logind, or empty.
QString logindSessionPath();

// Reads a whole sysfs attribute. Returns an empty optional-like pair where
// ok==false when the file is missing, which is how services detect "no battery"
// without throwing.
bool readSysfs(const QString &path, QString *out);
bool readSysfsInt(const QString &path, int *out);
bool writeSysfs(const QString &path, const QString &value);

} // namespace kapah::util