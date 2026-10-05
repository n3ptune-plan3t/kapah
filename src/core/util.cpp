#include "util.h"

#include "paths.h"

#include <QFile>
#include <QFontMetrics>
#include <QThread>

namespace kapah::util {

QString fold(const QString &s)
{
    QString out = s;
    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);
        if (c.isUpper()) {
            out[i] = c.toLower();
        }
    }
    return out;
}

QByteArray foldToUtf8(const QString &s)
{
    QByteArray out = s.toUtf8();
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool isSubsequence(QStringView haystackLower, QStringView needleLower)
{
    if (needleLower.isEmpty()) {
        return true;
    }
    qsizetype i = 0;
    for (const QChar c : haystackLower) {
        if (i < needleLower.size() && c == needleLower.at(i)) {
            ++i;
        }
    }
    return i == needleLower.size();
}

bool startsWithFold(QStringView haystack, QStringView needle)
{
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (qsizetype i = 0; i < needle.size(); ++i) {
        const QChar a = haystack.at(i);
        const QChar b = needle.at(i);
        if (a.isUpper()) {
            if (a.toLower() != b) {
                return false;
            }
        } else if (a != b) {
            return false;
        }
    }
    return true;
}

QString elide(const QString &text, int width, const QFontMetrics &fm)
{
    if (width <= 0) {
        return {};
    }
    if (fm.horizontalAdvance(text) <= width) {
        return text;
    }
    return fm.elidedText(text, Qt::ElideRight, width);
}

QString formatBytes(qint64 bytes)
{
    static const char *units[] = { "B", "kB", "MB", "GB" };
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 3) {
        v /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 %2").arg(bytes).arg(QLatin1String(units[unit]));
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

bool sleepMs(int ms)
{
    if (ms <= 0) {
        return true;
    }
    // Only used by tests and by PAM backoff; never on the UI hot path.
    return QThread::msleep(static_cast<unsigned long>(ms)) == 0;
}

QString logindSessionPath()
{
    const QString id = paths::logindSessionId();
    if (id.isEmpty()) {
        return {};
    }
    if (id.startsWith(QLatin1Char('/'))) {
        return id;
    }
    return QStringLiteral("/org/freedesktop/login1/session/") + id;
}

bool readSysfs(const QString &path, QString *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    // Sysfs attributes are single values; a 4 kB read never truncates a sane
    // one and avoids a stat + size probe.
    const QByteArray data = file.read(4096);
    if (out != nullptr) {
        *out = QString::fromUtf8(data).trimmed();
    }
    return true;
}

bool readSysfsInt(const QString &path, int *out)
{
    QString s;
    if (!readSysfs(path, &s)) {
        return false;
    }
    bool ok = false;
    const int v = s.toInt(&ok);
    if (ok && out != nullptr) {
        *out = v;
    }
    return ok;
}

bool writeSysfs(const QString &path, const QString &value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray data = value.toUtf8();
    return file.write(data) == data.size();
}

} // namespace kapah::util