#include "parser.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace minidb {

namespace {

const std::set<std::string>& Reserved() {
    static const std::set<std::string> kw = {
        "select", "from",   "where",  "group",   "by",     "having",  "order",  "limit",   "offset",
        "join",   "inner",  "cross",  "on",      "and",    "or",      "not",    "as",      "asc",
        "desc",   "insert", "into",   "values",  "create", "table",   "index",  "drop",    "delete",
        "update", "set",    "explain", "analyze", "null",  "is",      "true",   "false",   "distinct",
        "primary", "key",   "unique", "between", "in",     "like"};
    return kw;
}

}  // namespace

std::vector<Token> Tokenize(const std::string& sql) {
    std::vector<Token> out;
    size_t i = 0, n = sql.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(sql[i]);
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {  // -- comment
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }
        Token t;
        t.pos = i;
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
            t.type = TokenType::IDENT;
            for (size_t k = i; k < j; ++k) t.text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sql[k]))));
            i = j;
        } else if (c == '"') {  // quoted identifier keeps its case
            size_t j = i + 1;
            while (j < n && sql[j] != '"') ++j;
            if (j >= n) throw DbError("unterminated quoted identifier");
            t.type = TokenType::IDENT;
            t.text = sql.substr(i + 1, j - i - 1);
            i = j + 1;
        } else if (std::isdigit(c)) {
            size_t j = i;
            bool is_float = false;
            while (j < n && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
            if (j < n && sql[j] == '.' && j + 1 < n && std::isdigit(static_cast<unsigned char>(sql[j + 1]))) {
                is_float = true;
                ++j;
                while (j < n && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
            }
            if (j < n && (sql[j] == 'e' || sql[j] == 'E')) {
                size_t k = j + 1;
                if (k < n && (sql[k] == '+' || sql[k] == '-')) ++k;
                if (k < n && std::isdigit(static_cast<unsigned char>(sql[k]))) {
                    is_float = true;
                    j = k;
                    while (j < n && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
                }
            }
            t.type = is_float ? TokenType::FLOAT : TokenType::INTEGER;
            t.text = sql.substr(i, j - i);
            i = j;
        } else if (c == '\'') {  // string literal, '' is an escaped quote
            size_t j = i + 1;
            std::string s;
            while (true) {
                if (j >= n) throw DbError("unterminated string literal");
                if (sql[j] == '\'') {
                    if (j + 1 < n && sql[j + 1] == '\'') {
                        s.push_back('\'');
                        j += 2;
                        continue;
                    }
                    break;
                }
                s.push_back(sql[j++]);
            }
            t.type = TokenType::STRING;
            t.text = s;
            i = j + 1;
        } else {
            static const char* two[] = {"<=", ">=", "<>", "!=", "||"};
            t.type = TokenType::SYMBOL;
            bool matched = false;
            if (i + 1 < n) {
                for (const char* s : two) {
                    if (sql[i] == s[0] && sql[i + 1] == s[1]) {
                        t.text = s;
                        i += 2;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                if (std::string("(),;*=<>+-/%.").find(static_cast<char>(c)) == std::string::npos)
                    throw DbError(std::string("unexpected character '") + static_cast<char>(c) + "' at position " +
                                  std::to_string(i));
                t.text = std::string(1, static_cast<char>(c));
                ++i;
            }
        }
        out.push_back(std::move(t));
    }
    Token end;
    end.type = TokenType::END;
    end.pos = n;
    out.push_back(end);
    return out;
}

Parser::Parser(const std::string& sql) : toks_(Tokenize(sql)) {}

const Token& Parser::Peek(size_t k) const {
    size_t i = std::min(pos_ + k, toks_.size() - 1);
    return toks_[i];
}

Token Parser::Advance() {
    Token t = Peek();
    if (pos_ < toks_.size() - 1) ++pos_;
    return t;
}

bool Parser::CheckKw(const char* kw) const { return Peek().type == TokenType::IDENT && Peek().text == kw; }
bool Parser::AcceptKw(const char* kw) {
    if (!CheckKw(kw)) return false;
    Advance();
    return true;
}
void Parser::ExpectKw(const char* kw) {
    if (!AcceptKw(kw)) Error(std::string("expected ") + kw);
}
bool Parser::CheckSym(const char* s) const { return Peek().type == TokenType::SYMBOL && Peek().text == s; }
bool Parser::AcceptSym(const char* s) {
    if (!CheckSym(s)) return false;
    Advance();
    return true;
}
void Parser::ExpectSym(const char* s) {
    if (!AcceptSym(s)) Error(std::string("expected \"") + s + "\"");
}

std::string Parser::ExpectIdent() {
    const Token& t = Peek();
    if (t.type != TokenType::IDENT || Reserved().count(t.text)) Error("expected identifier");
    return Advance().text;
}

bool Parser::IsAliasToken() const {
    return Peek().type == TokenType::IDENT && !Reserved().count(Peek().text);
}

int64_t Parser::ExpectInteger() {
    if (Peek().type != TokenType::INTEGER) Error("expected integer");
    try {
        return std::stoll(Advance().text);
    } catch (...) {
        Error("integer out of range");
    }
}

void Parser::Error(const std::string& msg) const {
    const Token& t = Peek();
    std::string near = t.type == TokenType::END ? "end of input" : "\"" + t.text + "\"";
    throw DbError("syntax error: " + msg + " at or near " + near);
}

std::vector<Statement> Parser::ParseAll() {
    std::vector<Statement> out;
    while (true) {
        while (AcceptSym(";")) {
        }
        if (Peek().type == TokenType::END) break;
        out.push_back(ParseStatement());
        if (Peek().type != TokenType::END && !AcceptSym(";")) Error("expected \";\"");
    }
    return out;
}

Statement Parser::ParseStatement() {
    if (AcceptKw("create")) {
        bool unique = AcceptKw("unique");
        if (!unique && AcceptKw("table")) return ParseCreateTable();
        ExpectKw("index");
        return ParseCreateIndex(unique);
    }
    if (AcceptKw("drop")) {
        DropStmt d;
        if (AcceptKw("table")) {
            d.index = false;
        } else {
            ExpectKw("index");
            d.index = true;
        }
        d.name = ExpectIdent();
        return d;
    }
    if (AcceptKw("insert")) return ParseInsert();
    if (CheckKw("select")) return ParseSelect();
    if (AcceptKw("delete")) return ParseDelete();
    if (AcceptKw("update")) return ParseUpdate();
    if (AcceptKw("explain")) {
        ExplainStmt e;
        e.analyze = AcceptKw("analyze");
        if (!CheckKw("select")) Error("EXPLAIN supports only SELECT");
        e.select = std::make_shared<SelectStmt>(ParseSelect());
        return e;
    }
    Error("unknown statement");
}

TypeId Parser::ParseType() {
    if (Peek().type != TokenType::IDENT) Error("expected type name");
    std::string t = Advance().text;
    TypeId type;
    if (t == "int" || t == "integer" || t == "bigint" || t == "smallint")
        type = TypeId::INT;
    else if (t == "float" || t == "double" || t == "real" || t == "decimal" || t == "numeric") {
        type = TypeId::DOUBLE;
        AcceptKw("precision");
    } else if (t == "text" || t == "varchar" || t == "char" || t == "string")
        type = TypeId::TEXT;
    else if (t == "bool" || t == "boolean")
        type = TypeId::BOOL;
    else
        throw DbError("unknown type \"" + t + "\"");
    if (AcceptSym("(")) {  // VARCHAR(20), NUMERIC(10,2): accepted, size ignored
        ExpectInteger();
        if (AcceptSym(",")) ExpectInteger();
        ExpectSym(")");
    }
    return type;
}

CreateTableStmt Parser::ParseCreateTable() {
    CreateTableStmt c;
    c.name = ExpectIdent();
    ExpectSym("(");
    std::string table_pk;
    do {
        if (AcceptKw("primary")) {
            ExpectKw("key");
            ExpectSym("(");
            table_pk = ExpectIdent();
            ExpectSym(")");
            continue;
        }
        ColumnDef d;
        d.name = ExpectIdent();
        d.type = ParseType();
        while (true) {
            if (AcceptKw("primary")) {
                ExpectKw("key");
                d.primary_key = true;
                d.not_null = true;
            } else if (AcceptKw("not")) {
                ExpectKw("null");
                d.not_null = true;
            } else if (AcceptKw("unique")) {
                d.unique = true;
            } else if (AcceptKw("null")) {
            } else {
                break;
            }
        }
        c.cols.push_back(d);
    } while (AcceptSym(","));
    ExpectSym(")");
    if (!table_pk.empty()) {
        bool found = false;
        for (auto& d : c.cols) {
            if (d.name == table_pk) {
                d.primary_key = true;
                d.not_null = true;
                found = true;
            }
        }
        if (!found) throw DbError("primary key column \"" + table_pk + "\" does not exist");
    }
    return c;
}

CreateIndexStmt Parser::ParseCreateIndex(bool unique) {
    CreateIndexStmt c;
    c.unique = unique;
    c.name = ExpectIdent();
    ExpectKw("on");
    c.table = ExpectIdent();
    ExpectSym("(");
    c.column = ExpectIdent();
    ExpectSym(")");
    return c;
}

InsertStmt Parser::ParseInsert() {
    InsertStmt s;
    ExpectKw("into");
    s.table = ExpectIdent();
    if (AcceptSym("(")) {
        do {
            s.columns.push_back(ExpectIdent());
        } while (AcceptSym(","));
        ExpectSym(")");
    }
    if (CheckKw("select")) {
        s.select = std::make_shared<SelectStmt>(ParseSelect());
        return s;
    }
    ExpectKw("values");
    do {
        ExpectSym("(");
        std::vector<ExprPtr> row;
        do {
            row.push_back(ParseExpr());
        } while (AcceptSym(","));
        ExpectSym(")");
        s.rows.push_back(std::move(row));
    } while (AcceptSym(","));
    return s;
}

SelectStmt Parser::ParseSelect() {
    SelectStmt s;
    ExpectKw("select");
    s.distinct = AcceptKw("distinct");
    do {
        SelectItem item;
        if (AcceptSym("*")) {
            item.star = true;
        } else if (Peek().type == TokenType::IDENT && Peek(1).type == TokenType::SYMBOL && Peek(1).text == "." &&
                   Peek(2).type == TokenType::SYMBOL && Peek(2).text == "*") {
            item.star = true;
            item.star_table = Advance().text;
            Advance();
            Advance();
        } else {
            item.expr = ParseExpr();
            if (AcceptKw("as"))
                item.alias = ExpectIdent();
            else if (IsAliasToken())
                item.alias = Advance().text;
        }
        s.items.push_back(std::move(item));
    } while (AcceptSym(","));

    std::vector<ExprPtr> join_conds;
    if (AcceptKw("from")) {
        auto parse_ref = [&]() {
            TableRef r;
            r.name = ExpectIdent();
            if (AcceptKw("as"))
                r.alias = ExpectIdent();
            else if (IsAliasToken())
                r.alias = Advance().text;
            s.from.push_back(r);
        };
        parse_ref();
        while (true) {
            if (AcceptSym(",")) {
                parse_ref();
            } else if (CheckKw("join") || CheckKw("inner") || CheckKw("cross")) {
                bool cross = AcceptKw("cross");
                if (!cross) AcceptKw("inner");
                ExpectKw("join");
                parse_ref();
                if (!cross) {
                    ExpectKw("on");
                    join_conds.push_back(ParseExpr());
                }
            } else {
                break;
            }
        }
    }
    if (AcceptKw("where")) join_conds.push_back(ParseExpr());
    for (auto& c : join_conds) s.where = s.where ? Expr::Binary(BinOp::AND, s.where, c) : c;

    if (AcceptKw("group")) {
        ExpectKw("by");
        do {
            s.group_by.push_back(ParseExpr());
        } while (AcceptSym(","));
    }
    if (AcceptKw("having")) s.having = ParseExpr();
    if (AcceptKw("order")) {
        ExpectKw("by");
        do {
            OrderItem o;
            o.expr = ParseExpr();
            if (AcceptKw("desc"))
                o.desc = true;
            else
                AcceptKw("asc");
            s.order_by.push_back(o);
        } while (AcceptSym(","));
    }
    if (AcceptKw("limit")) s.limit = ExpectInteger();
    if (AcceptKw("offset")) s.offset = ExpectInteger();
    return s;
}

DeleteStmt Parser::ParseDelete() {
    DeleteStmt d;
    ExpectKw("from");
    d.table = ExpectIdent();
    if (AcceptKw("where")) d.where = ParseExpr();
    return d;
}

UpdateStmt Parser::ParseUpdate() {
    UpdateStmt u;
    u.table = ExpectIdent();
    ExpectKw("set");
    do {
        std::string col = ExpectIdent();
        ExpectSym("=");
        u.sets.emplace_back(col, ParseExpr());
    } while (AcceptSym(","));
    if (AcceptKw("where")) u.where = ParseExpr();
    return u;
}

// ---- expressions (lowest to highest precedence) ----

Parser::NestingGuard::NestingGuard(Parser* p) : p_(p) {
    if (++p_->depth_ > kMaxNesting) {
        --p_->depth_;
        p_->Error("expression is too deeply nested");
    }
}

ExprPtr Parser::ParseExpr() {
    NestingGuard guard(this);
    return ParseOr();
}

ExprPtr Parser::ParseOr() {
    ExprPtr l = ParseAnd();
    while (AcceptKw("or")) l = Expr::Binary(BinOp::OR, l, ParseAnd());
    return l;
}

ExprPtr Parser::ParseAnd() {
    ExprPtr l = ParseNot();
    while (AcceptKw("and")) l = Expr::Binary(BinOp::AND, l, ParseNot());
    return l;
}

ExprPtr Parser::ParseNot() {
    if (AcceptKw("not")) {
        NestingGuard guard(this);
        return Expr::Unary(UnaryOp::NOT, ParseNot());
    }
    return ParseComparison();
}

ExprPtr Parser::ParseComparison() {
    ExprPtr l = ParseAdditive();
    struct {
        const char* sym;
        BinOp op;
    } ops[] = {{"=", BinOp::EQ}, {"!=", BinOp::NE}, {"<>", BinOp::NE}, {"<=", BinOp::LE},
               {">=", BinOp::GE}, {"<", BinOp::LT},  {">", BinOp::GT}};
    for (auto& o : ops) {
        if (AcceptSym(o.sym)) return Expr::Binary(o.op, l, ParseAdditive());
    }
    if (AcceptKw("is")) {
        bool neg = AcceptKw("not");
        ExpectKw("null");
        return Expr::IsNull(l, neg);
    }
    bool neg = AcceptKw("not");
    if (AcceptKw("between")) {  // x BETWEEN a AND b  ==>  x >= a AND x <= b
        ExprPtr lo = ParseAdditive();
        ExpectKw("and");
        ExprPtr hi = ParseAdditive();
        ExprPtr e = Expr::Binary(BinOp::AND, Expr::Binary(BinOp::GE, l, lo), Expr::Binary(BinOp::LE, l, hi));
        return neg ? Expr::Unary(UnaryOp::NOT, e) : e;
    }
    if (AcceptKw("in")) {  // x IN (a, b, c)  ==>  x = a OR x = b OR x = c
        ExpectSym("(");
        std::vector<ExprPtr> terms;
        do {
            terms.push_back(Expr::Binary(BinOp::EQ, l, ParseExpr()));
        } while (AcceptSym(","));
        ExpectSym(")");
        // OR the terms together as a balanced tree (pairing neighbours round by
        // round), so a list of N values is only log2(N) levels deep, not N.
        while (terms.size() > 1) {
            std::vector<ExprPtr> next;
            for (size_t i = 0; i + 1 < terms.size(); i += 2)
                next.push_back(Expr::Binary(BinOp::OR, terms[i], terms[i + 1]));
            if (terms.size() % 2) next.push_back(terms.back());
            terms = std::move(next);
        }
        return neg ? Expr::Unary(UnaryOp::NOT, terms[0]) : terms[0];
    }
    if (AcceptKw("like")) {
        ExprPtr e = Expr::Binary(BinOp::LIKE, l, ParseAdditive());
        return neg ? Expr::Unary(UnaryOp::NOT, e) : e;
    }
    if (neg) Error("expected BETWEEN, IN or LIKE after NOT");
    return l;
}

ExprPtr Parser::ParseAdditive() {
    ExprPtr l = ParseMultiplicative();
    while (true) {
        if (AcceptSym("+"))
            l = Expr::Binary(BinOp::ADD, l, ParseMultiplicative());
        else if (AcceptSym("-"))
            l = Expr::Binary(BinOp::SUB, l, ParseMultiplicative());
        else
            return l;
    }
}

ExprPtr Parser::ParseMultiplicative() {
    ExprPtr l = ParseUnary();
    while (true) {
        if (AcceptSym("*"))
            l = Expr::Binary(BinOp::MUL, l, ParseUnary());
        else if (AcceptSym("/"))
            l = Expr::Binary(BinOp::DIV, l, ParseUnary());
        else if (AcceptSym("%"))
            l = Expr::Binary(BinOp::MOD, l, ParseUnary());
        else
            return l;
    }
}

ExprPtr Parser::ParseUnary() {
    if (AcceptSym("-")) {
        NestingGuard guard(this);
        // INT64_MIN is the one integer whose magnitude does not fit as a positive literal.
        if (Peek().type == TokenType::INTEGER && Peek().text == "9223372036854775808") {
            Advance();
            return Expr::Const(Value::Int(INT64_MIN));
        }
        ExprPtr x = ParseUnary();
        if (x->kind == ExprKind::CONST && x->value.Type() == TypeId::INT)
            return Expr::Const(Value::Int(checked::Neg(x->value.AsInt())));
        if (x->kind == ExprKind::CONST && x->value.Type() == TypeId::DOUBLE)
            return Expr::Const(Value::Double(-x->value.AsDouble()));
        return Expr::Unary(UnaryOp::NEG, x);
    }
    if (AcceptSym("+")) {
        NestingGuard guard(this);
        return ParseUnary();
    }
    return ParsePrimary();
}

ExprPtr Parser::ParsePrimary() {
    const Token& t = Peek();
    switch (t.type) {
        case TokenType::INTEGER: {
            std::string s = Advance().text;
            try {
                return Expr::Const(Value::Int(std::stoll(s)));
            } catch (...) {
                throw DbError("integer out of range: " + s);
            }
        }
        case TokenType::FLOAT: {
            std::string s = Advance().text;
            try {
                return Expr::Const(Value::Double(std::stod(s)));
            } catch (...) {
                throw DbError("value out of range: " + s);
            }
        }
        case TokenType::STRING: return Expr::Const(Value::Text(Advance().text));
        case TokenType::SYMBOL:
            if (AcceptSym("(")) {
                ExprPtr e = ParseExpr();
                ExpectSym(")");
                return e;
            }
            Error("unexpected symbol");
        case TokenType::IDENT: {
            if (AcceptKw("null")) return Expr::Const(Value::Null());
            if (AcceptKw("true")) return Expr::Const(Value::Bool(true));
            if (AcceptKw("false")) return Expr::Const(Value::Bool(false));
            if (Peek(1).type == TokenType::SYMBOL && Peek(1).text == "(") {
                static const std::pair<const char*, AggFunc> aggs[] = {{"count", AggFunc::COUNT},
                                                                       {"sum", AggFunc::SUM},
                                                                       {"avg", AggFunc::AVG},
                                                                       {"min", AggFunc::MIN},
                                                                       {"max", AggFunc::MAX}};
                for (auto& [name, f] : aggs) {
                    if (t.text == name) {
                        Advance();
                        ExpectSym("(");
                        if (f == AggFunc::COUNT && AcceptSym("*")) {
                            ExpectSym(")");
                            return Expr::Agg(f, nullptr, true);
                        }
                        ExprPtr arg = ParseExpr();
                        ExpectSym(")");
                        return Expr::Agg(f, arg, false);
                    }
                }
                Error("unknown function");
            }
            std::string first = ExpectIdent();
            if (AcceptSym(".")) return Expr::Col(first, ExpectIdent());
            return Expr::Col("", first);
        }
        case TokenType::END: Error("unexpected end of input");
    }
    Error("unexpected token");
}

}  // namespace minidb
