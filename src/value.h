// value.h - SQL values, column/schema descriptions and tuple (de)serialization.
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace minidb {

enum class TypeId : uint8_t { NULL_T = 0, BOOL = 1, INT = 2, DOUBLE = 3, TEXT = 4 };

const char* TypeName(TypeId t);

// A single SQL value (dynamically typed). INT is 64-bit.
class Value {
public:
    Value() = default;
    static Value Null() { return Value(); }
    static Value Bool(bool b) {
        Value v;
        v.type_ = TypeId::BOOL;
        v.i_ = b ? 1 : 0;
        return v;
    }
    static Value Int(int64_t i) {
        Value v;
        v.type_ = TypeId::INT;
        v.i_ = i;
        return v;
    }
    static Value Double(double d) {
        Value v;
        v.type_ = TypeId::DOUBLE;
        v.d_ = d;
        return v;
    }
    static Value Text(std::string s) {
        Value v;
        v.type_ = TypeId::TEXT;
        v.s_ = std::move(s);
        return v;
    }

    TypeId Type() const { return type_; }
    bool IsNull() const { return type_ == TypeId::NULL_T; }
    bool IsNumeric() const { return type_ == TypeId::INT || type_ == TypeId::DOUBLE; }

    int64_t AsInt() const;
    double AsDouble() const;
    bool AsBool() const;
    const std::string& AsText() const;

    std::string ToString() const;

    // Three-way comparison of two non-NULL comparable values: <0, 0, >0.
    int Compare(const Value& o) const;

    // Convert to the given column type (used when storing). Throws on mismatch.
    Value CastTo(TypeId t) const;

    // Appends a canonical binary encoding (for hash tables: GROUP BY, hash join, DISTINCT).
    void AppendKey(std::string* out) const;

private:
    TypeId type_ = TypeId::NULL_T;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
};

using Row = std::vector<Value>;

struct Column {
    std::string name;
    TypeId type = TypeId::INT;
    std::string table;  // qualifier (table name or alias), may be empty
    bool not_null = false;
};

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> cols) : cols_(std::move(cols)) {}

    size_t Size() const { return cols_.size(); }
    const Column& At(size_t i) const { return cols_[i]; }
    const std::vector<Column>& Columns() const { return cols_; }

    // Resolve "table.name" (table may be empty). Returns -1 if not found,
    // throws if the unqualified name matches more than one column.
    int Find(const std::string& table, const std::string& name) const;

    Schema WithQualifier(const std::string& q) const;
    static Schema Concat(const Schema& a, const Schema& b);

private:
    std::vector<Column> cols_;
};

// Tuple format on disk:
//   [null bitmap: ceil(n/8) bytes][field 0][field 1]...
//   INT/DOUBLE = 8 bytes, BOOL = 1 byte, TEXT = uint16 length + bytes. NULL fields take no space.
std::string SerializeTuple(const Schema& schema, const Row& row);
Row DeserializeTuple(const Schema& schema, const char* data, size_t len);

}  // namespace minidb
