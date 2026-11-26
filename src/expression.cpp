#include "expression.h"

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
            return value.Type() == TypeId::TEXT ? "'" + value.AsText() + "'" : value.ToString();
        case ExprKind::COLUMN: return table.empty() ? column : table + "." + column;
        case ExprKind::BINARY: return wrap(left) + " " + BinOpSymbol(op) + " " + wrap(right);
        case ExprKind::UNARY: return (uop == UnaryOp::NOT ? "NOT " : "-") + wrap(left);
        case ExprKind::IS_NULL: return wrap(left) + (negated ? " IS NOT NULL" : " IS NULL");
        case ExprKind::AGG: {
            static const char* names[] = {"count", "sum", "avg", "min", "max"};
            std::string n = names[static_cast<int>(agg)];
            return n + "(" + (agg_star ? std::string("*") : (agg_distinct ? "DISTINCT " : "") + left->ToString()) + ")";
        }
        case ExprKind::FUNC: {
            std::string s;
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

Value Arithmetic(BinOp op, const Value& l, const Value& r) {
    if (l.IsNull() || r.IsNull()) return Value::Null();
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
