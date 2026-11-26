#include "value.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace minidb {

// ---- calendar ----
// Howard Hinnant's days_from_civil / civil_from_days algorithms.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<int64_t>(yoe) + era * 400 + (*m <= 2);
}

namespace {

const int64_t kMinDays = DaysFromCivil(1, 1, 1);
const int64_t kMaxDays = DaysFromCivil(9999, 12, 31);

unsigned DaysInMonth(int64_t y, unsigned m) {
    static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : kDays[m - 1];
}

// Reads exactly `width` digits at s[*pos].
bool ReadDigits(const std::string& s, size_t* pos, size_t width, unsigned* out) {
    if (*pos + width > s.size()) return false;
    unsigned v = 0;
    for (size_t i = 0; i < width; ++i) {
        char c = s[*pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    *pos += width;
    *out = v;
    return true;
}

bool Expect(const std::string& s, size_t* pos, char c) {
    if (*pos >= s.size() || s[*pos] != c) return false;
    ++*pos;
    return true;
}

// 'YYYY-MM-DD' at the start of s; on success *pos is just past it.
bool ParseDatePart(const std::string& s, size_t* pos, int64_t* days) {
    unsigned y, m, d;
    if (!ReadDigits(s, pos, 4, &y) || !Expect(s, pos, '-') || !ReadDigits(s, pos, 2, &m) || !Expect(s, pos, '-') ||
        !ReadDigits(s, pos, 2, &d))
        return false;
    if (y < 1 || m < 1 || m > 12 || d < 1 || d > DaysInMonth(y, m)) return false;
    *days = DaysFromCivil(y, m, d);
    return true;
}

std::string Pad(int64_t v, int width) {
    std::string s = std::to_string(v);
    return std::string(s.size() < static_cast<size_t>(width) ? static_cast<size_t>(width) - s.size() : 0, '0') + s;
}

std::string FormatDate(int64_t days) {
    int64_t y;
    unsigned m, d;
    CivilFromDays(days, &y, &m, &d);
    return Pad(y, 4) + "-" + Pad(m, 2) + "-" + Pad(d, 2);
}

int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

std::string FormatTimestamp(int64_t secs) {
    int64_t days = FloorDiv(secs, kSecondsPerDay);
    int64_t rest = secs - days * kSecondsPerDay;
    return FormatDate(days) + " " + Pad(rest / 3600, 2) + ":" + Pad(rest / 60 % 60, 2) + ":" + Pad(rest % 60, 2);
}

}  // namespace

int64_t ParseDate(const std::string& s) {
    size_t pos = 0;
    int64_t days;
    if (!ParseDatePart(s, &pos, &days) || pos != s.size())
        throw DbError("invalid DATE value: '" + s + "' (expected YYYY-MM-DD)");
    return days;
}

int64_t ParseTimestamp(const std::string& s) {
    size_t pos = 0;
    int64_t days;
    bool ok = ParseDatePart(s, &pos, &days);
    unsigned h = 0, mi = 0, sec = 0;
    if (ok && pos < s.size()) {
        ok = (Expect(s, &pos, ' ') || Expect(s, &pos, 'T')) && ReadDigits(s, &pos, 2, &h) && Expect(s, &pos, ':') &&
             ReadDigits(s, &pos, 2, &mi);
        if (ok && pos < s.size()) ok = Expect(s, &pos, ':') && ReadDigits(s, &pos, 2, &sec);
        ok = ok && pos == s.size() && h < 24 && mi < 60 && sec < 60;
    }
    if (!ok) throw DbError("invalid TIMESTAMP value: '" + s + "' (expected YYYY-MM-DD HH:MM:SS)");
    return days * kSecondsPerDay + h * 3600 + mi * 60 + sec;
}

Value Value::Date(int64_t days) {
    if (days < kMinDays || days > kMaxDays) throw DbError("DATE out of range (years 1 to 9999)");
    Value v;
    v.type_ = TypeId::DATE;
    v.i_ = days;
    return v;
}

Value Value::Timestamp(int64_t seconds) {
    if (seconds < kMinDays * kSecondsPerDay || seconds > (kMaxDays + 1) * kSecondsPerDay - 1)
        throw DbError("TIMESTAMP out of range (years 1 to 9999)");
    Value v;
    v.type_ = TypeId::TIMESTAMP;
    v.i_ = seconds;
    return v;
}

int64_t Value::AsDays() const {
    if (type_ != TypeId::DATE) throw DbError(std::string("expected DATE, got ") + TypeName(type_));
    return i_;
}

int64_t Value::AsSeconds() const {
    if (type_ == TypeId::TIMESTAMP) return i_;
    if (type_ == TypeId::DATE) return i_ * kSecondsPerDay;
    throw DbError(std::string("expected TIMESTAMP, got ") + TypeName(type_));
}

std::string Value::ToSqlLiteral() const {
    switch (type_) {
        case TypeId::TEXT: {
            std::string out = "'";
            for (char c : s_) {
                if (c == '\'') out += '\'';
                out += c;
            }
            return out + "'";
        }
        case TypeId::DATE: return "DATE '" + ToString() + "'";
        case TypeId::TIMESTAMP: return "TIMESTAMP '" + ToString() + "'";
        default: return ToString();
    }
}

const char* TypeName(TypeId t) {
    switch (t) {
        case TypeId::NULL_T: return "NULL";
        case TypeId::BOOL: return "BOOL";
        case TypeId::INT: return "INT";
        case TypeId::DOUBLE: return "DOUBLE";
        case TypeId::TEXT: return "TEXT";
        case TypeId::DATE: return "DATE";
        case TypeId::TIMESTAMP: return "TIMESTAMP";
    }
    return "?";
}

int64_t Value::AsInt() const {
    switch (type_) {
        case TypeId::INT:
        case TypeId::BOOL: return i_;
        case TypeId::DOUBLE: return checked::FromDouble(d_);
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
        case TypeId::DATE: return FormatDate(i_);
        case TypeId::TIMESTAMP: return FormatTimestamp(i_);
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
    if (IsTemporal(type_) || IsTemporal(o.type_)) {
        // DATE vs TIMESTAMP compares the date as its midnight; TEXT is read as
        // the other side's type, so  d >= '2024-01-01'  works.
        auto secs = [](const Value& v, TypeId other) -> std::optional<int64_t> {
            if (IsTemporal(v.type_)) return v.AsSeconds();
            if (v.type_ == TypeId::TEXT)
                return other == TypeId::DATE ? ParseDate(v.s_) * kSecondsPerDay : ParseTimestamp(v.s_);
            return std::nullopt;
        };
        auto a = secs(*this, o.type_), b = secs(o, type_);
        if (a && b) return *a < *b ? -1 : (*a > *b ? 1 : 0);
    }
    throw DbError(std::string("cannot compare ") + TypeName(type_) + " with " + TypeName(o.type_));
}

Value Value::CastTo(TypeId t) const {
    if (IsNull() || type_ == t) return *this;
    if (t == TypeId::DOUBLE && type_ == TypeId::INT) return Double(static_cast<double>(i_));
    if (t == TypeId::INT && type_ == TypeId::DOUBLE) return Int(checked::FromDouble(d_));
    if (type_ == TypeId::TEXT && t == TypeId::DATE) return Date(ParseDate(s_));
    if (type_ == TypeId::TEXT && t == TypeId::TIMESTAMP) return Timestamp(ParseTimestamp(s_));
    if (type_ == TypeId::DATE && t == TypeId::TIMESTAMP) return Timestamp(i_ * kSecondsPerDay);
    if (type_ == TypeId::TIMESTAMP && t == TypeId::DATE) return Date(FloorDiv(i_, kSecondsPerDay));
    throw DbError(std::string("type mismatch: cannot store ") + TypeName(type_) + " value '" + ToString() +
                  "' in " + TypeName(t) + " column");
}

void Value::AppendKey(std::string* out) const {
    auto put64 = [out](uint64_t x) { out->append(reinterpret_cast<const char*>(&x), 8); };
    // A DATE hashes as its midnight TIMESTAMP, matching Compare().
    if (type_ == TypeId::DATE) {
        out->push_back(static_cast<char>(TypeId::TIMESTAMP));
        put64(static_cast<uint64_t>(i_ * kSecondsPerDay));
        return;
    }
    // Normalize integral doubles to INT so that 1 = 1.0 hashes identically.
    if (type_ == TypeId::DOUBLE && std::floor(d_) == d_ && std::fabs(d_) < 9e18) {
        out->push_back(static_cast<char>(TypeId::INT));
        put64(static_cast<uint64_t>(static_cast<int64_t>(d_)));
        return;
    }
    out->push_back(static_cast<char>(type_));
    switch (type_) {
        case TypeId::NULL_T: break;
        case TypeId::DATE:
        case TypeId::TIMESTAMP:
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
            case TypeId::DATE:
            case TypeId::TIMESTAMP: {
                Value c = v.CastTo(schema.At(i).type);
                int64_t x = schema.At(i).type == TypeId::DATE ? c.AsDays() : c.AsSeconds();
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
            case TypeId::DATE:
            case TypeId::TIMESTAMP: {
                need(8);
                auto x = ReadAs<int64_t>(data + off);
                row.push_back(schema.At(i).type == TypeId::DATE ? Value::Date(x) : Value::Timestamp(x));
                off += 8;
                break;
            }
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
