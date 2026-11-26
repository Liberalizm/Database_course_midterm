#include "value.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace minidb {

const char* TypeName(TypeId t) {
    switch (t) {
        case TypeId::NULL_T: return "NULL";
        case TypeId::BOOL: return "BOOL";
        case TypeId::INT: return "INT";
        case TypeId::DOUBLE: return "DOUBLE";
        case TypeId::TEXT: return "TEXT";
    }
    return "?";
}

int64_t Value::AsInt() const {
    switch (type_) {
        case TypeId::INT:
        case TypeId::BOOL: return i_;
        case TypeId::DOUBLE: return static_cast<int64_t>(d_);
        default: throw DbError(std::string("cannot use ") + TypeName(type_) + " as INT");
    }
}

double Value::AsDouble() const {
    switch (type_) {
        case TypeId::INT: return static_cast<double>(i_);
        case TypeId::DOUBLE: return d_;
        default: throw DbError(std::string("cannot use ") + TypeName(type_) + " as DOUBLE");
    }
}

bool Value::AsBool() const {
    if (type_ != TypeId::BOOL) throw DbError(std::string("expected BOOL, got ") + TypeName(type_));
    return i_ != 0;
}

const std::string& Value::AsText() const {
    if (type_ != TypeId::TEXT) throw DbError(std::string("expected TEXT, got ") + TypeName(type_));
    return s_;
}

std::string Value::ToString() const {
    switch (type_) {
        case TypeId::NULL_T: return "NULL";
        case TypeId::BOOL: return i_ ? "true" : "false";
        case TypeId::INT: return std::to_string(i_);
        case TypeId::DOUBLE: {
            std::ostringstream os;
            os << std::setprecision(12) << d_;
            return os.str();
        }
        case TypeId::TEXT: return s_;
    }
    return "?";
}

int Value::Compare(const Value& o) const {
    if (IsNumeric() && o.IsNumeric()) {
        if (type_ == TypeId::INT && o.type_ == TypeId::INT) return (i_ < o.i_) ? -1 : (i_ > o.i_ ? 1 : 0);
        double a = AsDouble(), b = o.AsDouble();
        return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    if (type_ == TypeId::TEXT && o.type_ == TypeId::TEXT) {
        int c = s_.compare(o.s_);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (type_ == TypeId::BOOL && o.type_ == TypeId::BOOL) return (i_ < o.i_) ? -1 : (i_ > o.i_ ? 1 : 0);
    throw DbError(std::string("cannot compare ") + TypeName(type_) + " with " + TypeName(o.type_));
}

Value Value::CastTo(TypeId t) const {
    if (IsNull() || type_ == t) return *this;
    if (t == TypeId::DOUBLE && type_ == TypeId::INT) return Double(static_cast<double>(i_));
    if (t == TypeId::INT && type_ == TypeId::DOUBLE) return Int(static_cast<int64_t>(d_));
    throw DbError(std::string("type mismatch: cannot store ") + TypeName(type_) + " value '" + ToString() +
                  "' in " + TypeName(t) + " column");
}

void Value::AppendKey(std::string* out) const {
    auto put64 = [out](uint64_t x) { out->append(reinterpret_cast<const char*>(&x), 8); };
    // Normalize integral doubles to INT so that 1 = 1.0 hashes identically.
    if (type_ == TypeId::DOUBLE && std::floor(d_) == d_ && std::fabs(d_) < 9e18) {
        out->push_back(static_cast<char>(TypeId::INT));
        put64(static_cast<uint64_t>(static_cast<int64_t>(d_)));
        return;
    }
    out->push_back(static_cast<char>(type_));
    switch (type_) {
        case TypeId::NULL_T: break;
        case TypeId::BOOL:
        case TypeId::INT: put64(static_cast<uint64_t>(i_)); break;
        case TypeId::DOUBLE: {
            uint64_t bits;
            std::memcpy(&bits, &d_, 8);
            put64(bits);
            break;
        }
        case TypeId::TEXT:
            put64(s_.size());
            out->append(s_);
            break;
    }
}

int Schema::Find(const std::string& table, const std::string& name) const {
    int found = -1;
    for (size_t i = 0; i < cols_.size(); ++i) {
        const Column& c = cols_[i];
        if (c.name != name) continue;
        if (!table.empty() && c.table != table) continue;
        if (found >= 0) throw DbError("column reference \"" + name + "\" is ambiguous");
        found = static_cast<int>(i);
    }
    return found;
}

Schema Schema::WithQualifier(const std::string& q) const {
    std::vector<Column> cols = cols_;
    for (auto& c : cols) c.table = q;
    return Schema(std::move(cols));
}

Schema Schema::Concat(const Schema& a, const Schema& b) {
    std::vector<Column> cols = a.cols_;
    cols.insert(cols.end(), b.cols_.begin(), b.cols_.end());
    return Schema(std::move(cols));
}

std::string SerializeTuple(const Schema& schema, const Row& row) {
    const size_t n = schema.Size();
    if (row.size() != n) throw DbError("internal: row/schema size mismatch");
    const size_t nb = (n + 7) / 8;
    std::string out(nb, '\0');
    for (size_t i = 0; i < n; ++i) {
        const Value& v = row[i];
        if (v.IsNull()) {
            out[i / 8] = static_cast<char>(out[i / 8] | (1 << (i % 8)));
            continue;
        }
        switch (schema.At(i).type) {
            case TypeId::INT: {
                int64_t x = v.AsInt();
                out.append(reinterpret_cast<const char*>(&x), 8);
                break;
            }
            case TypeId::DOUBLE: {
                double x = v.AsDouble();
                out.append(reinterpret_cast<const char*>(&x), 8);
                break;
            }
            case TypeId::BOOL: out.push_back(v.AsBool() ? 1 : 0); break;
            case TypeId::TEXT: {
                const std::string& s = v.AsText();
                if (s.size() > 65535) throw DbError("text value too long");
                auto len = static_cast<uint16_t>(s.size());
                out.append(reinterpret_cast<const char*>(&len), 2);
                out.append(s);
                break;
            }
            case TypeId::NULL_T: break;
        }
    }
    return out;
}

Row DeserializeTuple(const Schema& schema, const char* data, size_t len) {
    const size_t n = schema.Size();
    const size_t nb = (n + 7) / 8;
    if (len < nb) throw DbError("corrupt tuple");
    Row row;
    row.reserve(n);
    size_t off = nb;
    auto need = [&](size_t k) {
        if (off + k > len) throw DbError("corrupt tuple");
    };
    for (size_t i = 0; i < n; ++i) {
        if (data[i / 8] & (1 << (i % 8))) {
            row.push_back(Value::Null());
            continue;
        }
        switch (schema.At(i).type) {
            case TypeId::INT:
                need(8);
                row.push_back(Value::Int(ReadAs<int64_t>(data + off)));
                off += 8;
                break;
            case TypeId::DOUBLE:
                need(8);
                row.push_back(Value::Double(ReadAs<double>(data + off)));
                off += 8;
                break;
            case TypeId::BOOL:
                need(1);
                row.push_back(Value::Bool(data[off] != 0));
                off += 1;
                break;
            case TypeId::TEXT: {
                need(2);
                auto l = ReadAs<uint16_t>(data + off);
                off += 2;
                need(l);
                row.push_back(Value::Text(std::string(data + off, l)));
                off += l;
                break;
            }
            case TypeId::NULL_T: row.push_back(Value::Null()); break;
        }
    }
    return row;
}

}  // namespace minidb
