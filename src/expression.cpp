#include "expression.h"

#include "executors.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace minidb {

const char* BinOpSymbol(BinOp op) {
    switch (op) {
        case BinOp::EQ: return "=";
        case BinOp::NE: return "<>";
        case BinOp::LT: return "<";
        case BinOp::LE: return "<=";
        case BinOp::GT: return ">";
        case BinOp::GE: return ">=";
        case BinOp::AND: return "AND";
        case BinOp::OR: return "OR";
        case BinOp::ADD: return "+";
        case BinOp::SUB: return "-";
        case BinOp::MUL: return "*";
        case BinOp::DIV: return "/";
        case BinOp::MOD: return "%";
        case BinOp::LIKE: return "LIKE";
        case BinOp::CONCAT: return "||";
    }
    return "?";
}

std::string Expr::ToString() const {
    auto wrap = [](const ExprPtr& e) {
        std::string s = e->ToString();
        return e->kind == ExprKind::BINARY ? "(" + s + ")" : s;
    };
    switch (kind) {
        case ExprKind::CONST:
            return value.ToSqlLiteral();
        case ExprKind::COLUMN: return table.empty() ? column : table + "." + column;
        case ExprKind::BINARY: return wrap(left) + " " + BinOpSymbol(op) + " " + wrap(right);
        case ExprKind::UNARY: return (uop == UnaryOp::NOT ? "NOT " : "-") + wrap(left);
        case ExprKind::IS_NULL: return wrap(left) + (negated ? " IS NOT NULL" : " IS NULL");
        case ExprKind::AGG: {
            static const char* names[] = {"count", "sum", "avg", "min", "max"};
            std::string n = names[static_cast<int>(agg)];
            return n + "(" + (agg_star ? std::string("*") : (agg_distinct ? "DISTINCT " : "") + left->ToString()) + ")";
        }
        case ExprKind::SUBQUERY: {
            static const char* kinds[] = {"", "EXISTS ", ""};
            std::string s = std::string(negated ? "NOT " : "") + kinds[static_cast<int>(sub_kind)] + "(SELECT ...)";
            if (sub_kind == SubqueryKind::IN) s = wrap(left) + (negated ? " NOT IN " : " IN ") + "(SELECT ...)";
            return s;
        }
        case ExprKind::PARAM: return table.empty() ? column : table + "." + column;
        case ExprKind::SUBPLAN: {
            static const char* kinds[] = {"", "EXISTS ", ""};
            if (sub_kind == SubqueryKind::IN) return wrap(left) + (negated ? " NOT IN " : " IN ") + "(SELECT ...)";
            return std::string(negated ? "NOT " : "") + kinds[static_cast<int>(sub_kind)] + "(SELECT ...)";
        }
        case ExprKind::IN_SET:
            return wrap(left) + (negated ? " NOT IN " : " IN ") + "(" + std::to_string(in_set->size() + (in_set_has_null ? 1 : 0)) + " values)";
        case ExprKind::FUNC: {
            std::string s;
            if (func == "cast")
                return "CAST(" + args[0]->ToString() + " AS " + TypeName(static_cast<TypeId>(args[1]->value.AsInt())) + ")";
            if (func == "case") {
                s = "CASE";
                size_t i = 0;
                for (; i + 1 < args.size(); i += 2)
                    s += " WHEN " + args[i]->ToString() + " THEN " + args[i + 1]->ToString();
                if (i < args.size()) s += " ELSE " + args[i]->ToString();
                return s + " END";
            }
            for (size_t i = 0; i < args.size(); ++i) s += (i ? ", " : "") + args[i]->ToString();
            return func + "(" + s + ")";
        }
    }
    return "?";
}

ExprPtr BindExpr(const ExprPtr& e, const Schema& schema) {
    if (!e) return nullptr;
    auto c = std::make_shared<Expr>(*e);
    switch (e->kind) {
        case ExprKind::COLUMN: {
            if (e->prebound) return c;
            int idx = schema.Find(e->table, e->column);
            if (idx < 0) throw DbError("column \"" + e->ToString() + "\" does not exist");
            c->col_idx = idx;
            return c;
        }
        case ExprKind::AGG: throw DbError("aggregate functions are not allowed here");
        case ExprKind::SUBQUERY: throw DbError("subqueries are not allowed here");
        default:
            c->left = BindExpr(e->left, schema);
            c->right = BindExpr(e->right, schema);
            for (auto& a : c->args) a = BindExpr(a, schema);
            return c;
    }
}

bool ContainsAggregate(const Expr& e) {
    if (e.kind == ExprKind::AGG) return true;
    if ((e.left && ContainsAggregate(*e.left)) || (e.right && ContainsAggregate(*e.right))) return true;
    for (const auto& a : e.args)
        if (ContainsAggregate(*a)) return true;
    return false;
}

std::vector<ExprPtr> SplitConjuncts(const ExprPtr& e) {
    std::vector<ExprPtr> out;
    if (!e) return out;
    if (e->kind == ExprKind::BINARY && e->op == BinOp::AND) {
        auto l = SplitConjuncts(e->left), r = SplitConjuncts(e->right);
        out.insert(out.end(), l.begin(), l.end());
        out.insert(out.end(), r.begin(), r.end());
    } else {
        out.push_back(e);
    }
    return out;
}

ExprPtr MakeConjunction(const std::vector<ExprPtr>& parts) {
    ExprPtr out;
    for (const auto& p : parts) out = out ? Expr::Binary(BinOp::AND, out, p) : p;
    return out;
}

bool LikeMatch(const std::string& s, const std::string& p) {
    size_t si = 0, pi = 0, star = std::string::npos, match = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            match = si;
        } else if (pi < p.size() && (p[pi] == '_' || p[pi] == s[si])) {
            ++si;
            ++pi;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') ++pi;
    return pi == p.size();
}

namespace {

// DATE +/- INT days, TIMESTAMP +/- INT seconds, and the difference of two
// dates (in days) or of two timestamps (in seconds; a DATE counts as midnight).
Value TemporalArithmetic(BinOp op, const Value& l, const Value& r) {
    auto shift = [](const Value& t, int64_t n) {
        return t.Type() == TypeId::DATE ? Value::Date(checked::Add(t.AsDays(), n))
                                        : Value::Timestamp(checked::Add(t.AsSeconds(), n));
    };
    if (op == BinOp::ADD && IsTemporal(l.Type()) && r.Type() == TypeId::INT) return shift(l, r.AsInt());
    if (op == BinOp::ADD && l.Type() == TypeId::INT && IsTemporal(r.Type())) return shift(r, l.AsInt());
    if (op == BinOp::SUB && IsTemporal(l.Type()) && r.Type() == TypeId::INT) return shift(l, checked::Neg(r.AsInt()));
    if (op == BinOp::SUB && IsTemporal(l.Type()) && IsTemporal(r.Type())) {
        if (l.Type() == TypeId::DATE && r.Type() == TypeId::DATE) return Value::Int(checked::Sub(l.AsDays(), r.AsDays()));
        return Value::Int(checked::Sub(l.AsSeconds(), r.AsSeconds()));
    }
    throw DbError(std::string("operator ") + BinOpSymbol(op) + " is not defined for " + TypeName(l.Type()) + " and " +
                  TypeName(r.Type()));
}

Value Arithmetic(BinOp op, const Value& l, const Value& r) {
    if (l.IsNull() || r.IsNull()) return Value::Null();
    if (IsTemporal(l.Type()) || IsTemporal(r.Type())) return TemporalArithmetic(op, l, r);
    if (!l.IsNumeric() || !r.IsNumeric())
        throw DbError(std::string("operator ") + BinOpSymbol(op) + " requires numeric operands, got " +
                      TypeName(l.Type()) + " and " + TypeName(r.Type()));
    if (l.Type() == TypeId::INT && r.Type() == TypeId::INT) {
        int64_t a = l.AsInt(), b = r.AsInt();
        switch (op) {
            case BinOp::ADD: return Value::Int(checked::Add(a, b));
            case BinOp::SUB: return Value::Int(checked::Sub(a, b));
            case BinOp::MUL: return Value::Int(checked::Mul(a, b));
            case BinOp::DIV:
                if (b == 0) throw DbError("division by zero");
                return Value::Int(checked::Div(a, b));
            case BinOp::MOD:
                if (b == 0) throw DbError("division by zero");
                return Value::Int(checked::Mod(a, b));
            default: break;
        }
    }
    double a = l.AsDouble(), b = r.AsDouble();
    double res;
    switch (op) {
        case BinOp::ADD: res = a + b; break;
        case BinOp::SUB: res = a - b; break;
        case BinOp::MUL: res = a * b; break;
        case BinOp::DIV:
            if (b == 0) throw DbError("division by zero");
            res = a / b;
            break;
        case BinOp::MOD:
            if (b == 0) throw DbError("division by zero");
            res = std::fmod(a, b);
            break;
        default: throw DbError("internal: bad arithmetic operator");
    }
    // Like PostgreSQL: overflowing to infinity is an error, not a value.
    if (!std::isfinite(res)) throw DbError("value out of range: overflow");
    return Value::Double(res);
}

void CheckBool(const Value& v, const char* what) {
    if (!v.IsNull() && v.Type() != TypeId::BOOL)
        throw DbError(std::string("argument of ") + what + " must be boolean, not " + TypeName(v.Type()));
}

const std::string& TextArg(const Value& v, const std::string& fn) {
    if (v.Type() != TypeId::TEXT)
        throw DbError("function " + fn + " requires a TEXT argument, not " + TypeName(v.Type()));
    return v.AsText();
}

int64_t IntArg(const Value& v, const std::string& fn) {
    if (v.Type() != TypeId::INT) throw DbError("function " + fn + " requires an INT argument, not " + TypeName(v.Type()));
    return v.AsInt();
}

// CAST(v AS t). Unlike the implicit conversion on store (Value::CastTo), this
// also converts between TEXT and the other types.
Value CastValue(const Value& v, TypeId t) {
    if (v.IsNull() || v.Type() == t) return v;
    auto fail = [&]() -> Value {
        throw DbError(std::string("cannot cast ") + TypeName(v.Type()) + " value '" + v.ToString() + "' to " +
                      TypeName(t));
    };
    if (t == TypeId::TEXT) return Value::Text(v.ToString());
    if (v.Type() == TypeId::TEXT) {
        const std::string& s = v.AsText();
        size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        std::string x = b == std::string::npos ? "" : s.substr(b, e - b + 1);
        size_t used = 0;
        switch (t) {
            case TypeId::INT:
                try {
                    int64_t i = std::stoll(x, &used);
                    if (used == x.size()) return Value::Int(i);
                } catch (const std::exception&) {
                }
                return fail();
            case TypeId::DOUBLE:
                try {
                    double d = std::stod(x, &used);
                    if (used == x.size() && std::isfinite(d)) return Value::Double(d);
                } catch (const std::exception&) {
                }
                return fail();
            case TypeId::BOOL: {
                std::string l = x;
                for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (l == "true" || l == "t" || l == "1") return Value::Bool(true);
                if (l == "false" || l == "f" || l == "0") return Value::Bool(false);
                return fail();
            }
            case TypeId::DATE: return Value::Date(ParseDate(x));
            case TypeId::TIMESTAMP: return Value::Timestamp(ParseTimestamp(x));
            default: return fail();
        }
    }
    if (t == TypeId::BOOL && v.Type() == TypeId::INT) return Value::Bool(v.AsInt() != 0);
    if (t == TypeId::INT && v.Type() == TypeId::BOOL) return Value::Int(v.AsBool() ? 1 : 0);
    const bool numeric = (t == TypeId::INT || t == TypeId::DOUBLE) && v.IsNumeric();
    if (numeric || (IsTemporal(t) && IsTemporal(v.Type()))) return v.CastTo(t);
    return fail();
}

// Scalar functions and CASE. The parser has already checked the argument counts.
Value EvaluateFunction(const Expr& e, const Row& row) {
    const std::string& fn = e.func;
    const auto& args = e.args;

    // Functions that must not evaluate all their arguments up front.
    if (fn == "case") {
        size_t i = 0;
        for (; i + 1 < args.size(); i += 2) {
            Value c = Evaluate(*args[i], row);
            CheckBool(c, "CASE WHEN");
            if (!c.IsNull() && c.AsBool()) return Evaluate(*args[i + 1], row);
        }
        return i < args.size() ? Evaluate(*args[i], row) : Value::Null();
    }
    if (fn == "coalesce") {
        for (const auto& a : args) {
            Value v = Evaluate(*a, row);
            if (!v.IsNull()) return v;
        }
        return Value::Null();
    }

    std::vector<Value> v;
    for (const auto& a : args) v.push_back(Evaluate(*a, row));
    if (fn == "nullif") {
        if (!v[0].IsNull() && !v[1].IsNull() && v[0].Compare(v[1]) == 0) return Value::Null();
        return v[0];
    }
    for (const auto& x : v)  // every remaining function is NULL on NULL input
        if (x.IsNull()) return Value::Null();

    if (fn == "abs") {
        if (v[0].Type() == TypeId::INT) {
            int64_t x = v[0].AsInt();
            return Value::Int(x < 0 ? checked::Neg(x) : x);
        }
        if (v[0].Type() == TypeId::DOUBLE) return Value::Double(std::fabs(v[0].AsDouble()));
        throw DbError("function abs requires a numeric argument, not " + std::string(TypeName(v[0].Type())));
    }
    if (fn == "round") {
        int64_t digits = v.size() > 1 ? IntArg(v[1], fn) : 0;
        if (digits < -15 || digits > 15) throw DbError("round: number of digits must be between -15 and 15");
        if (v[0].Type() == TypeId::INT && digits >= 0) return v[0];
        if (!v[0].IsNumeric()) throw DbError("function round requires a numeric argument");
        double scale = std::pow(10.0, static_cast<double>(digits));
        double r = std::round(v[0].AsDouble() * scale) / scale;
        if (!std::isfinite(r)) throw DbError("value out of range: overflow");
        return v[0].Type() == TypeId::INT ? Value::Int(checked::FromDouble(r)) : Value::Double(r);
    }
    if (fn == "cast") return CastValue(v[0], static_cast<TypeId>(Evaluate(*args[1], row).AsInt()));
    if (fn == "date") {
        if (v[0].Type() != TypeId::TEXT && !IsTemporal(v[0].Type()))
            throw DbError("function date requires a TEXT, DATE or TIMESTAMP argument, not " +
                          std::string(TypeName(v[0].Type())));
        return v[0].CastTo(TypeId::DATE);
    }
    if (fn == "year" || fn == "month" || fn == "day" || fn == "hour" || fn == "minute" || fn == "second") {
        if (!IsTemporal(v[0].Type()))
            throw DbError("function " + fn + " requires a DATE or TIMESTAMP argument, not " +
                          std::string(TypeName(v[0].Type())));
        const int64_t secs = v[0].AsSeconds();
        const int64_t days = secs / kSecondsPerDay - (secs % kSecondsPerDay < 0);
        const int64_t in_day = secs - days * kSecondsPerDay;
        int64_t y;
        unsigned m, d;
        CivilFromDays(days, &y, &m, &d);
        if (fn == "year") return Value::Int(y);
        if (fn == "month") return Value::Int(m);
        if (fn == "day") return Value::Int(d);
        if (fn == "hour") return Value::Int(in_day / 3600);
        if (fn == "minute") return Value::Int(in_day / 60 % 60);
        return Value::Int(in_day % 60);
    }
    if (fn == "length") return Value::Int(static_cast<int64_t>(TextArg(v[0], fn).size()));
    if (fn == "upper" || fn == "lower") {
        std::string s = TextArg(v[0], fn);
        for (char& ch : s)
            ch = static_cast<char>(fn == "upper" ? std::toupper(static_cast<unsigned char>(ch))
                                                 : std::tolower(static_cast<unsigned char>(ch)));
        return Value::Text(std::move(s));
    }
    if (fn == "substr") {  // 1-based like SQL; start may lie before the string
        const std::string& s = TextArg(v[0], fn);
        int64_t start = IntArg(v[1], fn);
        int64_t end = static_cast<int64_t>(s.size()) + 1;  // exclusive, 1-based
        if (v.size() > 2) {
            int64_t len = IntArg(v[2], fn);
            if (len < 0) throw DbError("negative substring length not allowed");
            end = std::min(end, start > INT64_MAX - len ? INT64_MAX : start + len);
        }
        start = std::max<int64_t>(start, 1);
        if (start >= end) return Value::Text("");
        return Value::Text(s.substr(static_cast<size_t>(start - 1), static_cast<size_t>(end - start)));
    }
    throw DbError("internal: unknown function " + fn);
}

// A correlated subquery, evaluated for one outer row: copy the outer values the
// subquery refers to into its parameter slots, then run its plan again.
Value EvaluateSubplan(const Expr& e, const Row& row) {
    Value x;  // IN: the tested value, from the outer row
    if (e.sub_kind == SubqueryKind::IN) x = Evaluate(*e.left, row);
    for (size_t i = 0; i < e.args.size(); ++i) (*e.params)[i] = Evaluate(*e.args[i], row);
    Executor& plan = *e.subplan;
    plan.Init();
    Row r;
    switch (e.sub_kind) {
        case SubqueryKind::EXISTS: return Value::Bool(plan.Next(&r) != e.negated);
        case SubqueryKind::SCALAR: {
            if (!plan.Next(&r)) return Value::Null();
            Value v = r[0];
            if (plan.Next(&r)) throw DbError("more than one row returned by a subquery used as an expression");
            return v;
        }
        case SubqueryKind::IN: {
            // Same three-valued logic as IN_SET: TRUE on a match; else NULL if x
            // or any result is NULL; else FALSE (and the reverse for NOT IN).
            bool any = false, saw_null = false;
            while (plan.Next(&r)) {
                any = true;
                if (r[0].IsNull()) {
                    saw_null = true;
                } else if (!x.IsNull() && x.Compare(r[0]) == 0) {
                    return Value::Bool(!e.negated);
                }
            }
            if (!any) return Value::Bool(e.negated);
            if (x.IsNull() || saw_null) return Value::Null();
            return Value::Bool(e.negated);
        }
    }
    throw DbError("internal: bad subquery kind");
}

}  // namespace

Value Evaluate(const Expr& e, const Row& row) {
    switch (e.kind) {
        case ExprKind::CONST: return e.value;
        case ExprKind::COLUMN:
            if (e.col_idx < 0 || static_cast<size_t>(e.col_idx) >= row.size())
                throw DbError("internal: unbound column " + e.ToString());
            return row[static_cast<size_t>(e.col_idx)];
        case ExprKind::UNARY: {
            Value v = Evaluate(*e.left, row);
            if (v.IsNull()) return v;
            if (e.uop == UnaryOp::NOT) {
                CheckBool(v, "NOT");
                return Value::Bool(!v.AsBool());
            }
            if (v.Type() == TypeId::INT) return Value::Int(checked::Neg(v.AsInt()));
            if (v.Type() == TypeId::DOUBLE) return Value::Double(-v.AsDouble());
            throw DbError("unary minus requires a numeric operand");
        }
        case ExprKind::IS_NULL: return Value::Bool(Evaluate(*e.left, row).IsNull() != e.negated);
        case ExprKind::AGG: throw DbError("internal: aggregate evaluated as scalar");
        case ExprKind::FUNC: return EvaluateFunction(e, row);
        case ExprKind::SUBQUERY: throw DbError("internal: subquery was not resolved by the planner");
        case ExprKind::PARAM: return (*e.params)[static_cast<size_t>(e.param_slot)];
        case ExprKind::SUBPLAN: return EvaluateSubplan(e, row);
        case ExprKind::IN_SET: {
            // x IN (subquery results), three-valued: TRUE on a match; otherwise NULL if x
            // or any result is NULL (the answer is unknown); otherwise FALSE.
            if (e.in_set->empty() && !e.in_set_has_null) return Value::Bool(e.negated);
            Value x = Evaluate(*e.left, row);
            if (x.IsNull()) return Value::Null();
            std::string k;
            x.AppendKey(&k);
            if (e.in_set->count(k)) return Value::Bool(!e.negated);
            if (e.in_set_has_null) return Value::Null();
            return Value::Bool(e.negated);
        }
        case ExprKind::BINARY: break;
    }

    // Logical operators with short-circuit and three-valued logic.
    if (e.op == BinOp::AND || e.op == BinOp::OR) {
        const char* name = e.op == BinOp::AND ? "AND" : "OR";
        Value l = Evaluate(*e.left, row);
        CheckBool(l, name);
        bool dominant = (e.op == BinOp::OR);  // TRUE dominates OR, FALSE dominates AND
        if (!l.IsNull() && l.AsBool() == dominant) return Value::Bool(dominant);
        Value r = Evaluate(*e.right, row);
        CheckBool(r, name);
        if (!r.IsNull() && r.AsBool() == dominant) return Value::Bool(dominant);
        if (l.IsNull() || r.IsNull()) return Value::Null();
        return Value::Bool(!dominant);
    }

    Value l = Evaluate(*e.left, row);
    Value r = Evaluate(*e.right, row);
    switch (e.op) {
        case BinOp::EQ:
        case BinOp::NE:
        case BinOp::LT:
        case BinOp::LE:
        case BinOp::GT:
        case BinOp::GE: {
            if (l.IsNull() || r.IsNull()) return Value::Null();
            int c = l.Compare(r);
            switch (e.op) {
                case BinOp::EQ: return Value::Bool(c == 0);
                case BinOp::NE: return Value::Bool(c != 0);
                case BinOp::LT: return Value::Bool(c < 0);
                case BinOp::LE: return Value::Bool(c <= 0);
                case BinOp::GT: return Value::Bool(c > 0);
                default: return Value::Bool(c >= 0);
            }
        }
        case BinOp::LIKE:
            if (l.IsNull() || r.IsNull()) return Value::Null();
            return Value::Bool(LikeMatch(l.AsText(), r.AsText()));
        case BinOp::CONCAT:  // like PostgreSQL: NULL if either side is NULL; other types print as text
            if (l.IsNull() || r.IsNull()) return Value::Null();
            return Value::Text(l.ToString() + r.ToString());
        default: return Arithmetic(e.op, l, r);
    }
}

}  // namespace minidb
