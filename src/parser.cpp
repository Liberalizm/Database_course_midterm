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
        "primary", "key",   "unique", "between", "in",     "like",    "left",   "right",   "full",
        "outer",  "case",   "when",    "then",   "else",    "end",     "exists", "union",
        "intersect", "except", "all"};
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
            if (j == i + 1) throw DbError("zero-length quoted identifier");
            t.type = TokenType::IDENT;
            t.text = sql.substr(i + 1, j - i - 1);
            t.quoted = true;
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

bool Parser::CheckKw(const char* kw) const {
    return Peek().type == TokenType::IDENT && !Peek().quoted && Peek().text == kw;
}
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
    if (t.type != TokenType::IDENT || (!t.quoted && Reserved().count(t.text))) Error("expected identifier");
    return Advance().text;
}

bool Parser::IsAliasToken() const {
    return Peek().type == TokenType::IDENT && (Peek().quoted || !Reserved().count(Peek().text));
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
    if (AcceptKw("alter")) {
        ExpectKw("table");
        return ParseAlterTable();
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
    if (Statement t; ParseTransaction(&t)) return t;
    Error("unknown statement");
}

// Transaction control, with the common synonyms of PostgreSQL / SQLite / MySQL:
//   BEGIN [TRANSACTION | WORK]      START TRANSACTION
//   COMMIT [TRANSACTION | WORK]     END [TRANSACTION | WORK]
//   ROLLBACK [TRANSACTION | WORK]   ABORT [TRANSACTION | WORK]
//   SAVEPOINT name   RELEASE [SAVEPOINT] name   ROLLBACK [TRANSACTION | WORK] TO [SAVEPOINT] name
// None of these words is reserved, so they still work as table or column names.
bool Parser::ParseTransaction(Statement* out) {
    using K = TransactionStmt::Kind;
    auto noise = [this] { return AcceptKw("transaction") || AcceptKw("work"); };
    TransactionStmt t;
    if (AcceptKw("begin")) {
        noise();
        t.kind = K::BEGIN;
    } else if (AcceptKw("start")) {
        ExpectKw("transaction");
        t.kind = K::BEGIN;
    } else if (AcceptKw("commit") || AcceptKw("end")) {
        noise();
        t.kind = K::COMMIT;
    } else if (AcceptKw("abort")) {
        noise();
        t.kind = K::ROLLBACK;
    } else if (AcceptKw("rollback")) {
        noise();
        t.kind = K::ROLLBACK;
        if (AcceptKw("to")) {
            AcceptKw("savepoint");
            t.kind = K::ROLLBACK_TO;
            t.savepoint = ExpectIdent();
        }
    } else if (AcceptKw("savepoint")) {
        t.kind = K::SAVEPOINT;
        t.savepoint = ExpectIdent();
    } else if (AcceptKw("release")) {
        // "RELEASE savepoint" alone names a savepoint called "savepoint".
        if (CheckKw("savepoint") && Peek(1).type == TokenType::IDENT) Advance();
        t.kind = K::RELEASE;
        t.savepoint = ExpectIdent();
    } else {
        return false;
    }
    *out = std::move(t);
    return true;
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

// name type [PRIMARY KEY | NOT NULL | NULL | UNIQUE]...
ColumnDef Parser::ParseColumnDef() {
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
        } else if (AcceptKw("default")) {
            // A full comparison would swallow e.g. "NOT NULL" after it, so the
            // default is an arithmetic / string expression (|| level).
            d.default_expr = ParseConcat();
        } else if (AcceptKw("null")) {
        } else {
            break;
        }
    }
    return d;
}

// After ALTER TABLE. ADD / DROP / RENAME / COLUMN / TO are matched in context
// only (not reserved), so existing columns with those names keep working.
AlterTableStmt Parser::ParseAlterTable() {
    AlterTableStmt a;
    a.table = ExpectIdent();
    if (AcceptKw("add")) {
        a.action = AlterTableStmt::Action::ADD_COLUMN;
        // "ADD COLUMN x INT" vs. a column that is itself named "column": "ADD column INT".
        if (CheckKw("column") && Peek(1).type == TokenType::IDENT && Peek(2).type == TokenType::IDENT) Advance();
        a.column = ParseColumnDef();
    } else if (AcceptKw("drop")) {
        a.action = AlterTableStmt::Action::DROP_COLUMN;
        if (CheckKw("column") && Peek(1).type == TokenType::IDENT) Advance();
        a.name = ExpectIdent();
    } else if (AcceptKw("rename")) {
        if (AcceptKw("to")) {
            a.action = AlterTableStmt::Action::RENAME_TABLE;
            a.new_name = ExpectIdent();
        } else {
            a.action = AlterTableStmt::Action::RENAME_COLUMN;
            if (CheckKw("column") && !(Peek(1).type == TokenType::IDENT && Peek(1).text == "to")) Advance();
            a.name = ExpectIdent();
            ExpectKw("to");
            a.new_name = ExpectIdent();
        }
    } else {
        Error("expected ADD, DROP or RENAME");
    }
    return a;
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
        c.cols.push_back(ParseColumnDef());
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

SelectStmt Parser::ParseSelectCore() {
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
            if (AcceptSym("(")) {  // derived table: (SELECT ...) [AS] alias
                if (!CheckKw("select")) Error("expected SELECT");
                r.subquery = std::make_shared<SelectStmt>(ParseSelect());
                ExpectSym(")");
                AcceptKw("as");
                if (!IsAliasToken()) Error("subquery in FROM must have an alias");
                r.alias = Advance().text;
                r.name = r.alias;
                s.from.push_back(r);
                return;
            }
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
            } else if (AcceptKw("left")) {
                AcceptKw("outer");
                ExpectKw("join");
                parse_ref();
                ExpectKw("on");
                s.from.back().left_join = true;
                s.from.back().on = ParseExpr();
            } else if (CheckKw("right") || CheckKw("full")) {
                Error("only INNER, CROSS and LEFT joins are supported (rewrite RIGHT JOIN as LEFT JOIN)");
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
    return s;
}

// A query expression: SELECT cores combined with UNION / EXCEPT (left to right)
// and INTERSECT (binds tighter, as in standard SQL), then ORDER BY / LIMIT /
// OFFSET for the whole result.
SelectStmt Parser::ParseSelect() {
    NestingGuard guard(this);
    auto compound = [](SetOp op, bool all, SelectStmt l, SelectStmt r) {
        SelectStmt c;
        c.set_op = op;
        c.set_all = all;
        c.set_lhs = std::make_shared<SelectStmt>(std::move(l));
        c.set_rhs = std::make_shared<SelectStmt>(std::move(r));
        return c;
    };
    auto parse_intersect = [&]() {
        SelectStmt l = ParseSelectCore();
        while (AcceptKw("intersect")) {
            bool all = AcceptKw("all");
            if (!all) AcceptKw("distinct");
            l = compound(SetOp::INTERSECT, all, std::move(l), ParseSelectCore());
        }
        return l;
    };
    SelectStmt s = parse_intersect();
    while (CheckKw("union") || CheckKw("except")) {
        SetOp op = AcceptKw("union") ? SetOp::UNION : SetOp::EXCEPT;
        if (op == SetOp::EXCEPT) ExpectKw("except");
        bool all = AcceptKw("all");
        if (!all) AcceptKw("distinct");
        s = compound(op, all, std::move(s), parse_intersect());
    }

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
    ExprPtr l = ParseConcat();
    struct {
        const char* sym;
        BinOp op;
    } ops[] = {{"=", BinOp::EQ}, {"!=", BinOp::NE}, {"<>", BinOp::NE}, {"<=", BinOp::LE},
               {">=", BinOp::GE}, {"<", BinOp::LT},  {">", BinOp::GT}};
    for (auto& o : ops) {
        if (AcceptSym(o.sym)) return Expr::Binary(o.op, l, ParseConcat());
    }
    if (AcceptKw("is")) {
        bool neg = AcceptKw("not");
        ExpectKw("null");
        return Expr::IsNull(l, neg);
    }
    bool neg = AcceptKw("not");
    if (AcceptKw("between")) {  // x BETWEEN a AND b  ==>  x >= a AND x <= b
        ExprPtr lo = ParseConcat();
        ExpectKw("and");
        ExprPtr hi = ParseConcat();
        ExprPtr e = Expr::Binary(BinOp::AND, Expr::Binary(BinOp::GE, l, lo), Expr::Binary(BinOp::LE, l, hi));
        return neg ? Expr::Unary(UnaryOp::NOT, e) : e;
    }
    if (AcceptKw("in")) {  // x IN (a, b, c)  ==>  x = a OR x = b OR x = c
        ExpectSym("(");
        if (CheckKw("select")) {  // x [NOT] IN (SELECT ...)
            auto sub = std::make_shared<SelectStmt>(ParseSelect());
            ExpectSym(")");
            return Expr::Subquery(SubqueryKind::IN, std::move(sub), l, neg);
        }
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
        ExprPtr e = Expr::Binary(BinOp::LIKE, l, ParseConcat());
        return neg ? Expr::Unary(UnaryOp::NOT, e) : e;
    }
    if (neg) Error("expected BETWEEN, IN or LIKE after NOT");
    return l;
}

ExprPtr Parser::ParseConcat() {
    ExprPtr l = ParseAdditive();
    while (AcceptSym("||")) l = Expr::Binary(BinOp::CONCAT, l, ParseAdditive());
    return l;
}

// After CASE:  WHEN c THEN v [...] [ELSE e] END   (searched CASE), or
//              x WHEN a THEN v [...] [ELSE e] END (simple CASE, becomes x = a).
ExprPtr Parser::ParseCase() {
    NestingGuard guard(this);
    ExprPtr subject;
    if (!CheckKw("when") && !CheckKw("end")) subject = ParseExpr();
    std::vector<ExprPtr> args;
    if (!CheckKw("when")) Error("expected WHEN");
    while (AcceptKw("when")) {
        ExprPtr cond = ParseExpr();
        args.push_back(subject ? Expr::Binary(BinOp::EQ, subject, cond) : cond);
        ExpectKw("then");
        args.push_back(ParseExpr());
    }
    if (AcceptKw("else")) args.push_back(ParseExpr());
    ExpectKw("end");
    return Expr::Func("case", std::move(args));
}

ExprPtr Parser::ParseFunctionCall() {
    NestingGuard guard(this);
    std::string name = Advance().text;
    ExpectSym("(");

    static const std::pair<const char*, AggFunc> aggs[] = {{"count", AggFunc::COUNT},
                                                           {"sum", AggFunc::SUM},
                                                           {"avg", AggFunc::AVG},
                                                           {"min", AggFunc::MIN},
                                                           {"max", AggFunc::MAX}};
    for (auto& [agg_name, f] : aggs) {
        if (name != agg_name) continue;
        if (f == AggFunc::COUNT && AcceptSym("*")) {
            ExpectSym(")");
            return Expr::Agg(f, nullptr, true);
        }
        bool distinct = AcceptKw("distinct");
        ExprPtr arg = ParseExpr();
        ExpectSym(")");
        return Expr::Agg(f, arg, false, distinct);
    }

    // Scalar functions: name, minimum and maximum number of arguments.
    struct Fn {
        const char* name;
        size_t min_args, max_args;
    };
    static const Fn fns[] = {{"abs", 1, 1},   {"round", 1, 2},     {"length", 1, 1}, {"upper", 1, 1},
                             {"lower", 1, 1}, {"substr", 2, 3},    {"coalesce", 1, 64},
                             {"nullif", 2, 2}};
    for (const Fn& fn : fns) {
        if (name != fn.name) continue;
        std::vector<ExprPtr> args;
        if (!CheckSym(")")) {
            do {
                args.push_back(ParseExpr());
            } while (AcceptSym(","));
        }
        ExpectSym(")");
        if (args.size() < fn.min_args || args.size() > fn.max_args)
            throw DbError("function " + name + " takes " +
                          (fn.min_args == fn.max_args ? std::to_string(fn.min_args)
                                                      : std::to_string(fn.min_args) + " to " + std::to_string(fn.max_args)) +
                          " argument(s), got " + std::to_string(args.size()));
        return Expr::Func(name, std::move(args));
    }
    throw DbError("function " + name + "() does not exist");
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
                if (CheckKw("select")) {  // scalar subquery
                    NestingGuard guard(this);
                    auto sub = std::make_shared<SelectStmt>(ParseSelect());
                    ExpectSym(")");
                    return Expr::Subquery(SubqueryKind::SCALAR, std::move(sub));
                }
                ExprPtr e = ParseExpr();
                ExpectSym(")");
                return e;
            }
            Error("unexpected symbol");
        case TokenType::IDENT: {
            if (AcceptKw("null")) return Expr::Const(Value::Null());
            if (AcceptKw("true")) return Expr::Const(Value::Bool(true));
            if (AcceptKw("false")) return Expr::Const(Value::Bool(false));
            if (AcceptKw("case")) return ParseCase();
            if (AcceptKw("exists")) {
                NestingGuard guard(this);
                ExpectSym("(");
                auto sub = std::make_shared<SelectStmt>(ParseSelect());
                ExpectSym(")");
                return Expr::Subquery(SubqueryKind::EXISTS, std::move(sub));
            }
            if (!t.quoted && Peek(1).type == TokenType::SYMBOL && Peek(1).text == "(") return ParseFunctionCall();
            std::string first = ExpectIdent();
            if (AcceptSym(".")) return Expr::Col(first, ExpectIdent());
            return Expr::Col("", first);
        }
        case TokenType::END: Error("unexpected end of input");
    }
    Error("unexpected token");
}

}  // namespace minidb
