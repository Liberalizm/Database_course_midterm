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
    size_t pos = 0;
};

std::vector<Token> Tokenize(const std::string& sql);

class Parser {
public:
    explicit Parser(const std::string& sql);
    std::vector<Statement> ParseAll();

private:
    Statement ParseStatement();
    CreateTableStmt ParseCreateTable();
    CreateIndexStmt ParseCreateIndex(bool unique);
    InsertStmt ParseInsert();
    SelectStmt ParseSelect();
    DeleteStmt ParseDelete();
    UpdateStmt ParseUpdate();
    TypeId ParseType();

    ExprPtr ParseExpr();
    ExprPtr ParseOr();
    ExprPtr ParseAnd();
    ExprPtr ParseNot();
    ExprPtr ParseComparison();
    ExprPtr ParseAdditive();
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

    std::vector<Token> toks_;
    size_t pos_ = 0;
};

}  // namespace minidb
