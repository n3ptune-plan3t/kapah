#include "passwordfield.h"

#include "theme.h"

#include <QPainter>
#include <QRect>

#include <cstring>

namespace kapah {

namespace {
void scrubString(QString &s)
{
    if (s.isEmpty()) {
        return;
    }
    char16_t *data = s.data();
    const qsizetype n = s.size();
#if defined(__GLIBC__) || defined(__FreeBSD__)
    explicit_bzero(data, static_cast<std::size_t>(n) * sizeof(char16_t));
#else
    volatile char16_t *v = data;
    for (qsizetype i = 0; i < n; ++i) {
        v[i] = 0;
    }
#endif
    s.clear();
}
} // namespace

PasswordField::~PasswordField()
{
    scrubString(m_buffer);
}

void PasswordField::insert(const QChar &c)
{
    m_buffer.append(c);
}

void PasswordField::backspace()
{
    if (!m_buffer.isEmpty()) {
        m_buffer.chop(1);
    }
}

void PasswordField::clear()
{
    scrubString(m_buffer);
}

QString PasswordField::takePassword()
{
    QString out = m_buffer;
    scrubString(m_buffer);
    return out;
}

void PasswordField::paint(QPainter &p, const QRect &rect, const Theme &theme) const
{
    p.setPen(Qt::NoPen);
    p.setBrush(theme.color(palette::Surface));
    p.drawRoundedRect(rect, theme.px(8), theme.px(8));

    // Bullets: one small filled circle per character, centered horizontally.
    // This reveals length, which is what every lock screen already reveals.
    const int r = theme.px(3);
    const int gap = theme.px(8);
    const int n = length();
    const int totalW = n * (2 * r) + qMax(0, n - 1) * gap;
    int x = rect.center().x() - totalW / 2 + r;
    p.setBrush(theme.color(palette::Foreground));
    for (int i = 0; i < n; ++i) {
        p.drawEllipse(QPoint(x, rect.center().y()), r, r);
    }
    if (n == 0) {
        p.setPen(theme.color(palette::Muted));
        auto f = theme.font(1);
        p.setFont(f);
        p.drawText(rect, Qt::AlignCenter, QStringLiteral("Type your password"));
    }
}

} // namespace kapah
