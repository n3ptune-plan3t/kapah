// Password entry for kapah-lock: custom-painted, no undo stack.
//
// SECURITY (docs/SECURITY.md, item S4):
//   A QLineEdit widget cannot be used. QLineEdit keeps every edit in an undo
//   stack you cannot clear, and its echo semantics are style-driven. This
//   field stores the live buffer in one QString, renders only filled-circle
//   bullets (which reveal length, not contents), and explicit_bzero's the
//   buffer on clear() and on destruction.
//
// Nothing here is a QWidget: the lock surface is a raw wl_surface, not a Qt
// window, so the field paints straight into the lock frame via QPainter.

#pragma once

#include <QString>

class QPainter;
class QRect;

namespace kapah {

class Theme;

class PasswordField {
public:
    PasswordField() = default;
    ~PasswordField();

    // Non-copyable: the buffer is a secret.
    PasswordField(const PasswordField &) = delete;
    PasswordField &operator=(const PasswordField &) = delete;

    void insert(const QChar &c);
    void backspace();
    void clear();
    int length() const { return m_buffer.size(); }
    bool isEmpty() const { return m_buffer.isEmpty(); }

    // Hands over a copy for authentication; the field's own copy is scrubbed
    // immediately. The caller scrubs its copy after the worker replies.
    QString takePassword();

    // Draws the pill + bullets. Bullets are hand-drawn circles so no font is
    // required to carry a "bullet" glyph.
    void paint(QPainter &p, const QRect &rect, const Theme &theme) const;

private:
    QString m_buffer;
};

} // namespace kapah
