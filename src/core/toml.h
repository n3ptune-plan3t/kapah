// Minimal TOML v1.0-subset parser.
//
// Why hand-written (ADR-005): the roadmap forbids QSettings and asks for a tiny
// fast parser. QSettings on Linux is an INI parser wrapped in QVariant, which
// costs roughly 90 kB of RSS per parse round and pulls QMetaType/QVariant
// boxing into every access. We need: tables, sub-tables, arrays of tables,
// strings, integers, floats, booleans and inline arrays. Nothing else.
//
// Unknown keys must warn but not fail (section 7), so the parser is lenient on
// read and reports a Vec<Diagnostic> rather than aborting.

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace kapah::toml {

class Value;
using ValuePtr = std::shared_ptr<const Value>;

enum class Type {
    Invalid,
    String,
    Integer,
    Float,
    Boolean,
    Array,
    Table,
};

class Value {
public:
    Value() = default;

    Type type() const { return m_type; }

    bool isValid() const { return m_type != Type::Invalid; }
    bool isString() const { return m_type == Type::String; }
    bool isInteger() const { return m_type == Type::Integer; }
    bool isFloat() const { return m_type == Type::Float; }
    bool isBoolean() const { return m_type == Type::Boolean; }
    bool isArray() const { return m_type == Type::Array; }
    bool isTable() const { return m_type == Type::Table; }

    // Accessors are total: asking for the wrong type yields the supplied
    // fallback rather than UB, which is what lets the config layer treat "user
    // wrote a string where an int goes" as a warned-about default rather than
    // a crash.
    QString toString(const QString &fallback = {}) const;
    qint64 toInteger(qint64 fallback = 0) const;
    double toFloat(double fallback = 0.0) const;
    bool toBoolean(bool fallback = false) const;
    QVector<ValuePtr> toArray() const;
    QHash<QString, ValuePtr> toTable() const;

    int line() const { return m_line; }
    int column() const { return m_column; }

private:
    friend class Parser;

    Type m_type = Type::Invalid;
    int m_line = 0;
    int m_column = 0;

    // Only one of these is ever populated, so a small tagged union would be
    // smaller; but a QVariant-shaped struct keeps the accessor code trivial and
    // the whole config is parsed once per reload, so clarity wins.
    QString m_string;
    qint64 m_integer = 0;
    double m_float = 0.0;
    bool m_boolean = false;
    QVector<ValuePtr> m_array;
    QHash<QString, ValuePtr> m_table;
};

struct Diagnostic {
    enum Severity { Warning, Error };
    Severity severity = Warning;
    int line = 0;
    int column = 0;
    QString message;

    QString format() const;
};

struct Document {
    ValuePtr root;
    QVector<Diagnostic> diagnostics;

    bool hasErrors() const;
};

// Parses `text`. Never throws. On a syntax error the returned Document has a
// null root and at least one Error diagnostic; on recoverable problems (unknown
// escape, duplicate key) it returns a partial tree plus Warnings.
Document parse(const QString &text);

// Convenience: parse a file. Returns hasErrors()==true and a null root when the
// file cannot be read.
Document parseFile(const QString &path);

} // namespace kapah::toml