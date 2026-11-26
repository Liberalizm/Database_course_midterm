#include "expression.h"

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
            return n + "(" + (agg_star ? std::string("*") : left->ToString()) + ")";
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
            return c;
    }
}

bool ContainsAggregate(const Expr& e) {
    if (e.kind == ExprKind::AGG) return true;
    return (e.left && ContainsAggregate(*e.left)) || (e.right && ContainsAggregate(*e.right));
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
        default: return Arithmetic(e.op, l, r);
    }
}

}  // namespace minidb
