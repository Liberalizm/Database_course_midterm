// Unit tests: expression binding and evaluation (built directly from AST nodes, no parser).
#include "expression.h"
#include "test_util.h"

namespace minidb {
namespace {

using namespace testing;

ExprPtr C(Value v) { return Expr::Const(std::move(v)); }
ExprPtr Bin(BinOp op, ExprPtr l, ExprPtr r) { return Expr::Binary(op, std::move(l), std::move(r)); }
Value Eval(const ExprPtr& e) { return Evaluate(*BindExpr(e, Schema{}), {}); }

TEST(Expression, IntegerArithmetic) {
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(I(2)), C(I(3)))).AsInt(), 5);
    EXPECT_EQ(Eval(Bin(BinOp::SUB, C(I(2)), C(I(3)))).AsInt(), -1);
    EXPECT_EQ(Eval(Bin(BinOp::MUL, C(I(4)), C(I(3)))).AsInt(), 12);
    EXPECT_EQ(Eval(Bin(BinOp::DIV, C(I(7)), C(I(2)))).AsInt(), 3);  // integer division
    EXPECT_EQ(Eval(Bin(BinOp::MOD, C(I(7)), C(I(3)))).AsInt(), 1);
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(I(2)), C(I(3)))).Type(), TypeId::INT);
}

TEST(Expression, MixedArithmeticPromotesToDouble) {
    Value v = Eval(Bin(BinOp::DIV, C(I(7)), C(D(2.0))));
    EXPECT_EQ(v.Type(), TypeId::DOUBLE);
    EXPECT_DOUBLE_EQ(v.AsDouble(), 3.5);
}

TEST(Expression, DivisionByZero) {
    EXPECT_THROW(Eval(Bin(BinOp::DIV, C(I(1)), C(I(0)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MOD, C(I(1)), C(I(0)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::DIV, C(D(1)), C(D(0)))), DbError);
}

TEST(Expression, ArithmeticTypeErrors) {
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(T("a")), C(I(1)))), DbError);
    EXPECT_THROW(Eval(Expr::Unary(UnaryOp::NEG, C(T("a")))), DbError);
}

TEST(Expression, NullPropagates) {
    EXPECT_TRUE(Eval(Bin(BinOp::ADD, C(N()), C(I(1)))).IsNull());
    EXPECT_TRUE(Eval(Bin(BinOp::EQ, C(N()), C(N()))).IsNull()) << "NULL = NULL is unknown";
    EXPECT_TRUE(Eval(Bin(BinOp::LT, C(I(1)), C(N()))).IsNull());
    EXPECT_TRUE(Eval(Expr::Unary(UnaryOp::NOT, C(N()))).IsNull());
    EXPECT_TRUE(Eval(Expr::Unary(UnaryOp::NEG, C(N()))).IsNull());
}

TEST(Expression, Comparisons) {
    EXPECT_TRUE(Eval(Bin(BinOp::EQ, C(I(1)), C(D(1.0)))).AsBool());
    EXPECT_TRUE(Eval(Bin(BinOp::NE, C(T("a")), C(T("b")))).AsBool());
    EXPECT_TRUE(Eval(Bin(BinOp::LE, C(I(2)), C(I(2)))).AsBool());
    EXPECT_FALSE(Eval(Bin(BinOp::GT, C(I(2)), C(I(2)))).AsBool());
    EXPECT_TRUE(Eval(Bin(BinOp::GE, C(T("b")), C(T("a")))).AsBool());
    EXPECT_THROW(Eval(Bin(BinOp::EQ, C(I(1)), C(T("1")))), DbError);
}

// SQL three-valued logic truth tables. 'n' = NULL.
struct LogicCase {
    char l, r;
    char and_result, or_result;
};

class ThreeValuedLogic : public ::testing::TestWithParam<LogicCase> {};

Value FromChar(char c) { return c == 'n' ? N() : B(c == 't'); }
char ToChar(const Value& v) { return v.IsNull() ? 'n' : (v.AsBool() ? 't' : 'f'); }

TEST_P(ThreeValuedLogic, AndOr) {
    const auto& p = GetParam();
    EXPECT_EQ(ToChar(Eval(Bin(BinOp::AND, C(FromChar(p.l)), C(FromChar(p.r))))), p.and_result);
    EXPECT_EQ(ToChar(Eval(Bin(BinOp::OR, C(FromChar(p.l)), C(FromChar(p.r))))), p.or_result);
}

INSTANTIATE_TEST_SUITE_P(TruthTable, ThreeValuedLogic,
                         ::testing::Values(LogicCase{'t', 't', 't', 't'}, LogicCase{'t', 'f', 'f', 't'},
                                           LogicCase{'t', 'n', 'n', 't'}, LogicCase{'f', 'f', 'f', 'f'},
                                           LogicCase{'f', 'n', 'f', 'n'}, LogicCase{'n', 'f', 'f', 'n'},
                                           LogicCase{'n', 't', 'n', 't'}, LogicCase{'n', 'n', 'n', 'n'}));

TEST(Expression, ShortCircuitSkipsRightSide) {
    ExprPtr boom = Bin(BinOp::DIV, C(I(1)), C(I(0)));  // would throw if evaluated
    EXPECT_FALSE(Eval(Bin(BinOp::AND, C(B(false)), Bin(BinOp::EQ, boom, C(I(1))))).AsBool());
    EXPECT_TRUE(Eval(Bin(BinOp::OR, C(B(true)), Bin(BinOp::EQ, boom, C(I(1))))).AsBool());
}

TEST(Expression, LogicRequiresBooleans) {
    EXPECT_THROW(Eval(Bin(BinOp::AND, C(I(1)), C(B(true)))), DbError);
    EXPECT_THROW(Eval(Expr::Unary(UnaryOp::NOT, C(I(1)))), DbError);
}

TEST(Expression, IsNull) {
    EXPECT_TRUE(Eval(Expr::IsNull(C(N()), false)).AsBool());
    EXPECT_FALSE(Eval(Expr::IsNull(C(I(0)), false)).AsBool());
    EXPECT_TRUE(Eval(Expr::IsNull(C(I(0)), true)).AsBool());
}

struct LikeCase {
    const char* s;
    const char* pattern;
    bool match;
};
class Like : public ::testing::TestWithParam<LikeCase> {};
TEST_P(Like, Matches) { EXPECT_EQ(LikeMatch(GetParam().s, GetParam().pattern), GetParam().match); }
INSTANTIATE_TEST_SUITE_P(Patterns, Like,
                         ::testing::Values(LikeCase{"abc", "abc", true}, LikeCase{"abc", "a%", true},
                                           LikeCase{"abc", "%c", true}, LikeCase{"abc", "%b%", true},
                                           LikeCase{"abc", "a_c", true}, LikeCase{"abc", "a_", false},
                                           LikeCase{"abc", "%", true}, LikeCase{"", "%", true},
                                           LikeCase{"", "_", false}, LikeCase{"abc", "ABC", false},
                                           LikeCase{"aaab", "%ab", true}, LikeCase{"abcabd", "%ab_", true},
                                           LikeCase{"mississippi", "%iss%ppi", true},
                                           LikeCase{"abc", "abcd", false}));

TEST(Expression, LikeEvaluation) {
    EXPECT_TRUE(Eval(Bin(BinOp::LIKE, C(T("hello")), C(T("h%o")))).AsBool());
    EXPECT_TRUE(Eval(Bin(BinOp::LIKE, C(N()), C(T("%")))).IsNull());
    EXPECT_THROW(Eval(Bin(BinOp::LIKE, C(I(1)), C(T("%")))), DbError);
}

TEST(Expression, BindResolvesColumnsWithoutMutatingInput) {
    Schema s({Col("a", TypeId::INT, "t"), Col("b", TypeId::INT, "t")});
    ExprPtr e = Bin(BinOp::MUL, Expr::Col("", "b"), Expr::Col("t", "a"));
    ExprPtr bound = BindExpr(e, s);
    EXPECT_EQ(bound->left->col_idx, 1);
    EXPECT_EQ(bound->right->col_idx, 0);
    EXPECT_EQ(e->left->col_idx, -1) << "binding must work on a copy";
    EXPECT_EQ(Evaluate(*bound, {I(6), I(7)}).AsInt(), 42);
}

TEST(Expression, BindErrors) {
    Schema s({Col("a", TypeId::INT, "t")});
    EXPECT_THROW(BindExpr(Expr::Col("", "zzz"), s), DbError);
    EXPECT_THROW(BindExpr(Expr::Col("other", "a"), s), DbError);
    EXPECT_THROW(BindExpr(Expr::Agg(AggFunc::COUNT, nullptr, true), s), DbError);
}

TEST(Expression, PreboundColumnsAreKept) {
    ExprPtr e = Expr::Col("", "#0");
    e->col_idx = 3;
    e->prebound = true;
    EXPECT_EQ(BindExpr(e, Schema{})->col_idx, 3);
}

TEST(Expression, ConjunctionHelpers) {
    ExprPtr a = C(B(true)), b = C(B(false)), c = C(N());
    auto parts = SplitConjuncts(Bin(BinOp::AND, Bin(BinOp::AND, a, b), c));
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], a);
    EXPECT_EQ(parts[2], c);
    EXPECT_EQ(SplitConjuncts(Bin(BinOp::OR, a, b)).size(), 1u);
    EXPECT_TRUE(SplitConjuncts(nullptr).empty());
    EXPECT_EQ(MakeConjunction({}), nullptr);
    EXPECT_EQ(SplitConjuncts(MakeConjunction(parts)).size(), 3u);
}

TEST(Expression, ContainsAggregate) {
    EXPECT_TRUE(ContainsAggregate(*Bin(BinOp::ADD, C(I(1)), Expr::Agg(AggFunc::SUM, Expr::Col("", "x"), false))));
    EXPECT_FALSE(ContainsAggregate(*Bin(BinOp::ADD, C(I(1)), Expr::Col("", "x"))));
}

TEST(Expression, ToString) {
    EXPECT_EQ(Bin(BinOp::EQ, Expr::Col("t", "name"), C(T("bob")))->ToString(), "t.name = 'bob'");
    EXPECT_EQ(Expr::IsNull(Expr::Col("", "x"), true)->ToString(), "x IS NOT NULL");
    EXPECT_EQ(Expr::Agg(AggFunc::MAX, Expr::Col("", "x"), false)->ToString(), "max(x)");
}

}  // namespace
}  // namespace minidb
