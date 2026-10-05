#include "toml.h"

#include "logging.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstdlib>

namespace kapah::toml {

namespace {

constexpr int kMaxDepth = 16;
constexpr int kMaxArrayElements = 4096;
constexpr int kMaxInlineLength = 8192;

bool isBareKeyChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-');
}

QString unescapeBasic(const QStringView in, bool *ok)
{
    QString out;
    out.reserve(in.size());
    *ok = true;

    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in[i];
        if (c != QLatin1Char('\\')) {
            out.append(c);
            continue;
        }
        if (++i >= in.size()) {
            *ok = false;
            return out;
        }
        switch (in[i].unicode()) {
        case 'b': out.append(QChar(0x08)); break;
        case 't': out.append(QChar(0x09)); break;
        case 'n': out.append(QChar(0x0A)); break;
        case 'f': out.append(QChar(0x0C)); break;
        case 'r': out.append(QChar(0x0D)); break;
        case '"': out.append(QLatin1Char('"')); break;
        case '\\': out.append(QLatin1Char('\\')); break;
        case 'u': {
            if (i + 4 >= in.size()) {
                *ok = false;
                return out;
            }
            bool converted = false;
            const uint cp = in.mid(i + 1, 4).toUInt(&converted, 16);
            if (!converted) {
                *ok = false;
                return out;
            }
            out.append(QString::fromUcs4(&cp, 1));
            i += 4;
            break;
        }
        case 'U': {
            if (i + 8 >= in.size()) {
                *ok = false;
                return out;
            }
            bool converted = false;
            const uint cp = in.mid(i + 1, 8).toUInt(&converted, 16);
            if (!converted || cp > 0x10FFFF) {
                *ok = false;
                return out;
            }
            out.append(QString::fromUcs4(&cp, 1));
            i += 8;
            break;
        }
        default:
            // Unknown escape: keep the character, warn. Being lenient here is
            // deliberate (section 7: a bad config must never prevent startup).
            out.append(in[i]);
            break;
        }
    }
    return out;
}

// TOML multi-line basic strings trim a leading newline after the opening
// delimiter and skip whitespace before a line-ending backslash.
QString processMultilineBasic(const QStringView in, bool *ok)
{
    QString out;
    *ok = true;
    int i = 0;

    if (in.startsWith(QLatin1String("\r\n"))) {
        i = 2;
    } else if (in.startsWith(QLatin1Char('\n'))) {
        i = 1;
    }

    while (i < in.size()) {
        const QChar c = in[i];
        if (c == QLatin1Char('\\')) {
            int j = i + 1;
            while (j < in.size() && (in[j] == QLatin1Char(' ') || in[j] == QLatin1Char('\t')
                                       || in[j] == QLatin1Char('\r'))) {
                ++j;
            }
            if (j < in.size() && in[j] == QLatin1Char('\n')) {
                i = j + 1;
                while (i < in.size()
                       && (in[i] == QLatin1Char(' ') || in[i] == QLatin1Char('\t')
                           || in[i] == QLatin1Char('\r') || in[i] == QLatin1Char('\n'))) {
                    ++i;
                }
                continue;
            }
            out.append(c);
            ++i;
            continue;
        }
        out.append(c);
        ++i;
    }
    return out;
}

class Parser {
public:
    explicit Parser(const QString &text)
        : m_text(text)
    {
    }

    Document run()
    {
        Document doc;
        auto root = std::make_shared<Value>();
        root->m_type = Type::Table;

        m_root = root.get();

        while (true) {
            skipWhitespaceAndComments();
            if (atEnd()) {
                break;
            }
            if (peek() == QLatin1Char('[')) {
                if (!parseTableHeader(doc.diagnostics)) {
                    break;
                }
                continue;
            }
            if (!parseKeyValue(doc.diagnostics)) {
                break;
            }
        }

        doc.root = root;
        return doc;
    }

private:
    const QString &m_text;
    int m_pos = 0;
    int m_line = 1;
    int m_col = 1;
    Value *m_root = nullptr;
    // Tables created by [a.b] headers, in creation order. Used to detect a table
    // that is both defined inline and reopened with a header.
    QStringList m_explicitTables;
    int m_depth = 0;

    bool atEnd() const { return m_pos >= m_text.size(); }
    QChar peek() const { return m_pos < m_text.size() ? m_text.at(m_pos) : QChar(); }
    QChar peekAt(int offset) const
    {
        return m_pos + offset < m_text.size() ? m_text.at(m_pos + offset) : QChar();
    }

    QChar advance()
    {
        const QChar c = m_text.at(m_pos++);
        if (c == QLatin1Char('\n')) {
            ++m_line;
            m_col = 1;
        } else {
            ++m_col;
        }
        return c;
    }

    bool match(QChar c)
    {
        if (peek() == c) {
            advance();
            return true;
        }
        return false;
    }

    void error(QVector<Diagnostic> &diags, const QString &msg)
    {
        diags.append({ Diagnostic::Error, m_line, m_col, msg });
    }

    void warn(QVector<Diagnostic> &diags, int line, int col, const QString &msg)
    {
        diags.append({ Diagnostic::Warning, line, col, msg });
    }

    void skipInlineWhitespace()
    {
        while (!atEnd() && (peek() == QLatin1Char(' ') || peek() == QLatin1Char('\t'))) {
            advance();
        }
    }

    void skipWhitespaceAndComments()
    {
        while (!atEnd()) {
            const QChar c = peek();
            if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r')) {
                advance();
                continue;
            }
            if (c == QLatin1Char('\n')) {
                advance();
                continue;
            }
            if (c == QLatin1Char('#')) {
                while (!atEnd() && peek() != QLatin1Char('\n')) {
                    advance();
                }
                continue;
            }
            break;
        }
    }

    void skipToEndOfLine()
    {
        while (!atEnd() && peek() != QLatin1Char('\n')) {
            advance();
        }
    }

    bool parseKey(QVector<Diagnostic> &diags, QStringList &parts)
    {
        skipInlineWhitespace();
        if (atEnd()) {
            error(diags, QStringLiteral("expected key, found end of input"));
            return false;
        }

        const QChar c = peek();
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            QString s;
            if (!parseBasicOrLiteralString(diags, s)) {
                return false;
            }
            parts.append(s);
            return true;
        }

        QString key;
        while (!atEnd() && isBareKeyChar(peek())) {
            key.append(advance());
        }
        if (key.isEmpty()) {
            error(diags, QStringLiteral("invalid key character '%1'").arg(c));
            return false;
        }
        parts.append(key);
        return true;
    }

    bool parseKeyPath(QVector<Diagnostic> &diags, QStringList &parts)
    {
        if (!parseKey(diags, parts)) {
            return false;
        }
        while (true) {
            skipInlineWhitespace();
            if (!match(QLatin1Char('.'))) {
                return true;
            }
            if (!parseKey(diags, parts)) {
                return false;
            }
        }
    }

    bool parseBasicOrLiteralString(QVector<Diagnostic> &diags, QString &out)
    {
        const int startLine = m_line;
        const int startCol = m_col;
        const QChar quote = advance();

        const bool triple = peek() == quote && peekAt(1) == quote;
        if (triple) {
            advance();
            advance();
        }

        QString raw;
        bool terminated = false;
        const bool literal = quote == QLatin1Char('\'');

        while (!atEnd()) {
            if (triple) {
                if (peek() == quote && peekAt(1) == quote && peekAt(2) == quote) {
                    advance();
                    advance();
                    advance();
                    terminated = true;
                    break;
                }
            } else {
                if (peek() == quote) {
                    advance();
                    terminated = true;
                    break;
                }
                if (peek() == QLatin1Char('\n')) {
                    break;
                }
            }

            if (!triple && !literal && peek() == QLatin1Char('\\')) {
                raw.append(advance());
                if (!atEnd()) {
                    raw.append(advance());
                }
                continue;
            }
            raw.append(advance());
        }

        if (!terminated) {
            error(diags, QStringLiteral("unterminated string starting at line %1 column %2")
                                     .arg(startLine)
                                     .arg(startCol));
            return false;
        }

        if (literal) {
            out = raw;
            return true;
        }

        bool ok = false;
        out = triple ? processMultilineBasic(raw, &ok) : unescapeBasic(raw, &ok);
        if (!ok) {
            warn(diags, m_line, m_col, QStringLiteral("invalid escape sequence in string literal"));
        }
        return true;
    }

    bool parseTableHeader(QVector<Diagnostic> &diags)
    {
        const int headerLine = m_line;
        advance(); // '['
        const bool arrayOfTables = match(QLatin1Char('['));
        if (arrayOfTables) {
            // '[[' -- we already consumed the second bracket? No: match consumed
            // the second '[', so the header is "[[name]]".
        }

        QStringList parts;
        if (!parseKeyPath(diags, parts)) {
            return false;
        }
        skipInlineWhitespace();
        if (!match(QLatin1Char(']'))) {
            error(diags, QStringLiteral("expected ']' to close table header"));
            return false;
        }
        if (arrayOfTables && !match(QLatin1Char(']'))) {
            error(diags, QStringLiteral("expected ']]' to close array-of-tables header"));
            return false;
        }

        const QString joined = parts.join(QLatin1Char('.'));
        Value *table = resolveTablePath(diags, parts, headerLine);
        if (table == nullptr) {
            return false;
        }

        if (arrayOfTables) {
            auto array = std::make_shared<Value>();
            array->m_type = Type::Array;
            array->m_line = headerLine;
            array->m_column = m_col;
            auto entry = std::make_shared<Value>();
            entry->m_type = Type::Table;
            entry->m_line = headerLine;
            entry->m_column = m_col;

            QHash<QString, ValuePtr> &parent = table->m_table;
            const QString leaf = parts.last();
            if (parent.contains(leaf)) {
                // An array of tables can be reopened; a table cannot be turned
                // into an array after the fact.
                if (parent.value(leaf)->type() != Type::Array) {
                    error(diags, QStringLiteral("'%1' is already defined as a non-array table")
                                         .arg(joined));
                    return false;
                }
                const_cast<ValuePtr &>(parent[leaf])->m_array.append(entry);
                m_root = entry.get();
            } else {
                array->m_array.append(entry);
                parent.insert(leaf, array);
                m_root = entry.get();
            }
            return true;
        }

        // Plain table header: appending is an error (TOML forbids it), silently
        // continuing would hide a real mistake in the user's config.
        if (m_explicitTables.contains(joined)) {
            warn(diags, headerLine, m_col,
                QStringLiteral("table '[%1]' defined more than once; contents merged")
                    .arg(joined));
        }
        m_explicitTables.append(joined);
        m_root = table;
        return true;
    }

    // Walks/creates the nested table chain for `parts` and returns the leaf.
    Value *resolveTablePath(QVector<Diagnostic> &diags, const QStringList &parts, int line)
    {
        Value *node = m_root;
        for (const QString &part : parts) {
            QHash<QString, ValuePtr> &map = node->m_table;
            auto it = map.constFind(part);
            if (it == map.constEnd()) {
                auto child = std::make_shared<Value>();
                child->m_type = Type::Table;
                child->m_line = line;
                child->m_column = m_col;
                map.insert(part, child);
                node = child.get();
                continue;
            }
            if ((*it)->type() == Type::Array) {
                // Dotted path into an array of tables: descend into its last
                // entry, which is what TOML specifies.
                const QVector<ValuePtr> &arr = (*it)->m_array;
                if (arr.isEmpty() || arr.last()->type() != Type::Table) {
                    error(diags, QStringLiteral("cannot use '%1' as a table").arg(part));
                    return nullptr;
                }
                node = arr.last().get();
                continue;
            }
            if ((*it)->type() != Type::Table) {
                error(diags,
                    QStringLiteral("'%1' is already defined as a value, not a table").arg(part));
                return nullptr;
            }
            node = it->get();
        }
        return node;
    }

    bool parseKeyValue(QVector<Diagnostic> &diags)
    {
        QStringList parts;
        if (!parseKeyPath(diags, parts)) {
            return false;
        }
        skipInlineWhitespace();
        if (!match(QLatin1Char('='))) {
            error(diags, QStringLiteral("expected '=' after key"));
            return false;
        }
        skipInlineWhitespace();

        auto value = parseValue(diags);
        if (!value) {
            return false;
        }

        Value *table = m_root;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            QHash<QString, ValuePtr> &map = table->m_table;
            const QString &part = parts.at(i);
            auto it = map.constFind(part);
            if (it == map.constEnd()) {
                auto child = std::make_shared<Value>();
                child->m_type = Type::Table;
                child->m_line = value->line();
                child->m_column = value->column();
                map.insert(part, child);
                table = child.get();
                continue;
            }
            if ((*it)->type() != Type::Table) {
                error(diags,
                    QStringLiteral("'%1' is already defined as a value, not a table").arg(part));
                return false;
            }
            table = it->get();
        }

        const QString leaf = parts.last();
        if (table->m_table.contains(leaf)) {
            warn(diags, value->line(), value->column(),
                QStringLiteral("duplicate key '%1'; the later value wins").arg(leaf));
        }
        table->m_table.insert(leaf, value);

        skipInlineWhitespace();
        if (!atEnd() && peek() != QLatin1Char('\n') && peek() != QLatin1Char('#')) {
            warn(diags, m_line, m_col,
                QStringLiteral("unexpected trailing characters after value; ignored"));
            skipToEndOfLine();
        }
        return true;
    }

    ValuePtr parseValue(QVector<Diagnostic> &diags)
    {
        const int vline = m_line;
        const int vcol = m_col;
        auto value = std::make_shared<Value>();
        value->m_line = vline;
        value->m_column = vcol;

        if (atEnd()) {
            error(diags, QStringLiteral("expected a value"));
            return nullptr;
        }

        const QChar c = peek();

        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            QString s;
            if (!parseBasicOrLiteralString(diags, s)) {
                return nullptr;
            }
            value->m_type = Type::String;
            value->m_string = s;
            return value;
        }

        if (c == QLatin1Char('[')) {
            if (++m_depth > kMaxDepth) {
                error(diags, QStringLiteral("array/table nesting too deep"));
                return nullptr;
            }
            advance();
            value->m_type = Type::Array;
            while (true) {
                skipWhitespaceAndComments();
                if (atEnd()) {
                    error(diags, QStringLiteral("unterminated array"));
                    --m_depth;
                    return nullptr;
                }
                if (peek() == QLatin1Char(']')) {
                    advance();
                    break;
                }
                auto item = parseValue(diags);
                if (!item) {
                    --m_depth;
                    return nullptr;
                }
                if (value->m_array.size() >= kMaxArrayElements) {
                    error(diags, QStringLiteral("array has more than %1 elements")
                                         .arg(kMaxArrayElements));
                    --m_depth;
                    return nullptr;
                }
                value->m_array.append(item);
                skipWhitespaceAndComments();
                if (match(QLatin1Char(','))) {
                    continue;
                }
            }
            --m_depth;
            return value;
        }

        if (c == QLatin1Char('{')) {
            if (++m_depth > kMaxDepth) {
                error(diags, QStringLiteral("array/table nesting too deep"));
                return nullptr;
            }
            advance();
            value->m_type = Type::Table;
            while (true) {
                skipWhitespaceAndComments();
                if (atEnd()) {
                    error(diags, QStringLiteral("unterminated inline table"));
                    --m_depth;
                    return nullptr;
                }
                if (peek() == QLatin1Char('}')) {
                    advance();
                    break;
                }
                QStringList keyParts;
                if (!parseKeyPath(diags, keyParts)) {
                    --m_depth;
                    return nullptr;
                }
                skipInlineWhitespace();
                if (!match(QLatin1Char('='))) {
                    error(diags, QStringLiteral("expected '=' in inline table"));
                    --m_depth;
                    return nullptr;
                }
                skipInlineWhitespace();
                auto item = parseValue(diags);
                if (!item) {
                    --m_depth;
                    return nullptr;
                }
                Value *node = value.get();
                for (int i = 0; i + 1 < keyParts.size(); ++i) {
                    auto child = std::make_shared<Value>();
                    child->m_type = Type::Table;
                    node->m_table.insert(keyParts.at(i), child);
                    node = child.get();
                }
                node->m_table.insert(keyParts.last(), item);
                skipInlineWhitespace();
                match(QLatin1Char(','));
            }
            --m_depth;
            return value;
        }

        // Bare token: true / false / number / date (we keep dates as strings).
        QString token;
        while (!atEnd()) {
            const QChar t = peek();
            if (t == QLatin1Char('\n') || t == QLatin1Char(',') || t == QLatin1Char(']')
                || t == QLatin1Char('}') || t == QLatin1Char('#')) {
                break;
            }
            token.append(advance());
            if (token.size() > 256) {
                error(diags, QStringLiteral("unterminated value token"));
                return nullptr;
            }
        }

        const QString trimmed = token.trimmed();
        if (trimmed.isEmpty()) {
            error(diags, QStringLiteral("expected a value"));
            return nullptr;
        }

        if (trimmed == QLatin1String("true")) {
            value->m_type = Type::Boolean;
            value->m_boolean = true;
            return value;
        }
        if (trimmed == QLatin1String("false")) {
            value->m_type = Type::Boolean;
            value->m_boolean = false;
            return value;
        }

        // Numbers, with TOML's `_` digit separators and 0x/0o/0b radix prefixes.
        QString numeric = trimmed;
        numeric.remove(QLatin1Char('_'));
        bool ok = false;

        int radix = 10;
        if (numeric.startsWith(QLatin1String("0x"))) {
            radix = 16;
            numeric = numeric.mid(2);
        } else if (numeric.startsWith(QLatin1String("0o"))) {
            radix = 8;
            numeric = numeric.mid(2);
        } else if (numeric.startsWith(QLatin1String("0b"))) {
            radix = 2;
            numeric = numeric.mid(2);
        }

        if (radix != 10) {
            const qlonglong v = numeric.toLongLong(&ok, radix);
            if (ok) {
                value->m_type = Type::Integer;
                value->m_integer = v;
                return value;
            }
        } else {
            const qlonglong iv = numeric.toLongLong(&ok, 10);
            if (ok) {
                value->m_type = Type::Integer;
                value->m_integer = iv;
                return value;
            }
            const double fv = numeric.toDouble(&ok);
            if (ok && std::isfinite(fv)) {
                value->m_type = Type::Float;
                value->m_float = fv;
                return value;
            }
            // inf / nan are accepted as floats.
            if (numeric == QLatin1String("inf") || numeric == QLatin1String("+inf")
                || numeric == QLatin1String("-inf") || numeric == QLatin1String("nan")) {
                value->m_type = Type::Float;
                value->m_float = numeric.toDouble();
                return value;
            }
        }

        // Not a number: keep it as a string. Dates and times land here. This is
        // the leniency section 7 asks for.
        value->m_type = Type::String;
        value->m_string = trimmed;
        return value;
    }
};

} // namespace

QString Value::toString(const QString &fallback) const
{
    if (m_type == Type::String) {
        return m_string;
    }
    if (m_type == Type::Integer) {
        return QString::number(m_integer);
    }
    if (m_type == Type::Float) {
        return QString::number(m_float);
    }
    if (m_type == Type::Boolean) {
        return m_boolean ? QStringLiteral("true") : QStringLiteral("false");
    }
    return fallback;
}

qint64 Value::toInteger(qint64 fallback) const
{
    if (m_type == Type::Integer) {
        return m_integer;
    }
    if (m_type == Type::Float) {
        return static_cast<qint64>(m_float);
    }
    if (m_type == Type::String) {
        bool ok = false;
        const qint64 v = m_string.toLongLong(&ok);
        if (ok) {
            return v;
        }
    }
    if (m_type == Type::Boolean) {
        return m_boolean ? 1 : 0;
    }
    return fallback;
}

double Value::toFloat(double fallback) const
{
    if (m_type == Type::Float) {
        return m_float;
    }
    if (m_type == Type::Integer) {
        return static_cast<double>(m_integer);
    }
    if (m_type == Type::String) {
        bool ok = false;
        const double v = m_string.toDouble(&ok);
        if (ok) {
            return v;
        }
    }
    return fallback;
}

bool Value::toBoolean(bool fallback) const
{
    if (m_type == Type::Boolean) {
        return m_boolean;
    }
    if (m_type == Type::Integer) {
        return m_integer != 0;
    }
    if (m_type == Type::String) {
        const QString s = m_string.toLower();
        if (s == QLatin1String("true") || s == QLatin1String("yes") || s == QLatin1String("on")
            || s == QLatin1String("1")) {
            return true;
        }
        if (s == QLatin1String("false") || s == QLatin1String("no") || s == QLatin1String("off")
            || s == QLatin1String("0")) {
            return false;
        }
    }
    return fallback;
}

QVector<ValuePtr> Value::toArray() const
{
    return m_type == Type::Array ? m_array : QVector<ValuePtr> {};
}

QHash<QString, ValuePtr> Value::toTable() const
{
    return m_type == Type::Table ? m_table : QHash<QString, ValuePtr> {};
}

QString Diagnostic::format() const
{
    return QStringLiteral("%1:%2: %3: %4")
        .arg(QString::fromLatin1(severity == Error ? "error" : "warning"))
        .arg(line)
        .arg(column)
        .arg(message);
}

bool Document::hasErrors() const
{
    for (const Diagnostic &d : diagnostics) {
        if (d.severity == Diagnostic::Error) {
            return true;
        }
    }
    return false;
}

Document parse(const QString &text)
{
    Parser parser(text);
    Document doc = parser.run();

    for (const Diagnostic &d : doc.diagnostics) {
        if (d.severity == Diagnostic::Error) {
            LOG_WARNF(Config, "%s", qPrintable(d.format()));
        } else {
            LOG_WARNF(Config, "%s", qPrintable(d.format()));
        }
    }

    return doc;
}

Document parseFile(const QString &path)
{
    QFile file(path);
    Document doc;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        doc.diagnostics.append({ Diagnostic::Error, 0, 0,
            QStringLiteral("cannot read '%1': %2").arg(path, file.errorString()) });
        return doc;
    }
    return parse(QString::fromUtf8(file.readAll()));
}

} // namespace kapah::toml