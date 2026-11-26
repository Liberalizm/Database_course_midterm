// Unit tests: SQL tokenizer.
#include "parser.h"
#include "test_util.h"

namespace minidb {
namespace {

std::vector<std::pair<TokenType, std::string>> Lex(const std::string& sql) {
    std::vector<std::pair<TokenType, std::string>> out;
    for (const auto& t : Tokenize(sql)) out.emplace_back(t.type, t.text);
    return out;
}

using TT = TokenType;

TEST(Lexer, EmptyInputYieldsOnlyEnd) {
    auto toks = Tokenize("   \n\t ");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].type, TT::END);
}

TEST(Lexer, IdentifiersAreLowercased) {
    auto t = Lex("SELECT Name FROM Users");
    ASSERT_EQ(t.size(), 5u);
    EXPECT_EQ(t[0], std::make_pair(TT::IDENT, std::string("select")));
    EXPECT_EQ(t[1].second, "name");
    EXPECT_EQ(t[3].second, "users");
}

TEST(Lexer, QuotedIdentifierKeepsCase) {
    auto t = Lex("\"MixedCase\"");
    EXPECT_EQ(t[0], std::make_pair(TT::IDENT, std::string("MixedCase")));
}

TEST(Lexer, Numbers) {
    auto t = Lex("42 3.14 1e3 2.5E-2 7.");
    EXPECT_EQ(t[0], std::make_pair(TT::INTEGER, std::string("42")));
    EXPECT_EQ(t[1], std::make_pair(TT::FLOAT, std::string("3.14")));
    EXPECT_EQ(t[2], std::make_pair(TT::FLOAT, std::string("1e3")));
    EXPECT_EQ(t[3], std::make_pair(TT::FLOAT, std::string("2.5E-2")));
    EXPECT_EQ(t[4], std::make_pair(TT::INTEGER, std::string("7")));  // "7." is 7 then '.'
    EXPECT_EQ(t[5], std::make_pair(TT::SYMBOL, std::string(".")));
}

TEST(Lexer, StringsWithEscapedQuotes) {
    auto t = Lex("'it''s' ''");
    EXPECT_EQ(t[0], std::make_pair(TT::STRING, std::string("it's")));
    EXPECT_EQ(t[1], std::make_pair(TT::STRING, std::string("")));
}

TEST(Lexer, StringsPreserveCaseAndSpaces) {
    EXPECT_EQ(Lex("'Hello World'")[0].second, "Hello World");
}

TEST(Lexer, TwoCharacterOperators) {
    auto t = Lex("<= >= <> != < > =");
    std::vector<std::string> ops;
    for (size_t i = 0; i + 1 < t.size(); ++i) ops.push_back(t[i].second);
    EXPECT_EQ(ops, (std::vector<std::string>{"<=", ">=", "<>", "!=", "<", ">", "="}));
}

TEST(Lexer, QualifiedColumn) {
    auto t = Lex("t.col");
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[0].first, TT::IDENT);
    EXPECT_EQ(t[1], std::make_pair(TT::SYMBOL, std::string(".")));
    EXPECT_EQ(t[2].first, TT::IDENT);
}

TEST(Lexer, CommentsAreSkipped) {
    auto t = Lex("SELECT -- a comment\n 1");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[1], std::make_pair(TT::INTEGER, std::string("1")));
}

TEST(Lexer, MinusIsNotAComment) {
    auto t = Lex("5-3");
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[1].second, "-");
}

TEST(Lexer, TokenPositions) {
    auto toks = Tokenize("ab  cd");
    EXPECT_EQ(toks[0].pos, 0u);
    EXPECT_EQ(toks[1].pos, 4u);
}

TEST(Lexer, Errors) {
    EXPECT_THROW(Tokenize("'unterminated"), DbError);
    EXPECT_THROW(Tokenize("\"unterminated"), DbError);
    EXPECT_THROW(Tokenize("SELECT @x"), DbError);
}

}  // namespace
}  // namespace minidb
