// parser.h - SQL lexer and recursive-descent parser.
#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace minidb {

enum class TokenType { IDENT, INTEGER, FLOAT, STRING, SYMBOL, END };

struct Token {
    TokenType type = TokenType::END;
    std::string text;  // identifiers / keywords are lower-cased
    bool quoted = false;  // "quoted identifier": never a keyword, keeps its case
    size_t pos = 0;  // the token is sql[pos, end)
    size_t end = 0;
};

std::vector<Token> Tokenize(const std::string& sql);

class Parser {
public:
    explicit Parser(const std::string& sql);
    std::vector<Statement> ParseAll();

    // Parses text that must be exactly one SELECT query (a view's definition).
    static SelectStmt ParseQuery(const std::string& sql);

private:
    Statement ParseStatement();
    CreateTableStmt ParseCreateTable();
    CreateViewStmt ParseCreateView(bool or_replace);
    ColumnDef ParseColumnDef();
    AlterTableStmt ParseAlterTable();
    CreateIndexStmt ParseCreateIndex(bool unique);
    InsertStmt ParseInsert();
    SelectStmt ParseSelect();      // full query: cores + UNION/INTERSECT/EXCEPT + ORDER BY/LIMIT
    SelectStmt ParseSelectCore();  // SELECT ... FROM ... WHERE ... GROUP BY ... HAVING ...
    DeleteStmt ParseDelete();
    UpdateStmt ParseUpdate();
    TypeId ParseType();

    ExprPtr ParseExpr();
    ExprPtr ParseOr();
    ExprPtr ParseAnd();
    ExprPtr ParseNot();
    ExprPtr ParseComparison();
    ExprPtr ParseConcat();  // a || b  (binds looser than + and -, tighter than comparisons)
    ExprPtr ParseAdditive();
    ExprPtr ParseCase();
    ExprPtr ParseFunctionCall();
    ExprPtr ParseMultiplicative();
    ExprPtr ParseUnary();
    ExprPtr ParsePrimary();

    const Token& Peek(size_t k = 0) const;
    Token Advance();
    bool CheckKw(const char* kw) const;
    bool AcceptKw(const char* kw);
    void ExpectKw(const char* kw);
    bool CheckSym(const char* s) const;
    bool AcceptSym(const char* s);
    void ExpectSym(const char* s);
    std::string ExpectIdent();
    bool IsAliasToken() const;
    int64_t ExpectInteger();
    [[noreturn]] void Error(const std::string& msg) const;

    // Recursive descent uses ~10 stack frames per nesting level, so input like
    // "((((...))))" or "- - - ... 1" must be cut off before the stack runs out.
    static constexpr int kMaxNesting = 256;
    struct NestingGuard {
        explicit NestingGuard(Parser* p);
        ~NestingGuard() { --p_->depth_; }
        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;
        Parser* p_;
    };

    std::string sql_;
    std::vector<Token> toks_;
    size_t pos_ = 0;
    int depth_ = 0;
};

}  // namespace minidb
