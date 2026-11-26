// Unit tests: expression binding and evaluation (built directly from AST nodes, no parser).
#include <optional>
#include <unordered_set>

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

// Signed overflow is undefined behaviour in C++, and INT64_MIN / -1 even kills
// the process on x86, so every overflow must surface as an SQL error instead.
TEST(Expression, IntegerOverflowIsAnError) {
    const int64_t max = INT64_MAX, min = INT64_MIN;
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(I(max)), C(I(1)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(I(min)), C(I(-1)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::SUB, C(I(min)), C(I(1)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::SUB, C(I(0)), C(I(min)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(I(max)), C(I(2)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(I(min)), C(I(-1)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(I(-1)), C(I(min)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(I(4294967296)), C(I(4294967296)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::DIV, C(I(min)), C(I(-1)))), DbError);
    EXPECT_THROW(Eval(Expr::Unary(UnaryOp::NEG, C(I(min)))), DbError);
}

TEST(Expression, IntegerArithmeticAtTheLimits) {
    const int64_t max = INT64_MAX, min = INT64_MIN;
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(I(max - 1)), C(I(1)))).AsInt(), max);
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(I(min)), C(I(max)))).AsInt(), -1);
    EXPECT_EQ(Eval(Bin(BinOp::SUB, C(I(-1)), C(I(max)))).AsInt(), min);
    EXPECT_EQ(Eval(Bin(BinOp::MUL, C(I(min / 2)), C(I(2)))).AsInt(), min);
    EXPECT_EQ(Eval(Bin(BinOp::MUL, C(I(-3037000499)), C(I(-3037000499)))).AsInt(), 9223372030926249001);
    EXPECT_EQ(Eval(Bin(BinOp::MUL, C(I(max)), C(I(-1)))).AsInt(), -max);
    EXPECT_EQ(Eval(Bin(BinOp::DIV, C(I(min)), C(I(1)))).AsInt(), min);
    EXPECT_EQ(Eval(Bin(BinOp::MOD, C(I(min)), C(I(-1)))).AsInt(), 0) << "traps on x86 if done naively";
    EXPECT_EQ(Eval(Bin(BinOp::MOD, C(I(-7)), C(I(3)))).AsInt(), -1) << "sign follows the dividend";
    EXPECT_EQ(Eval(Expr::Unary(UnaryOp::NEG, C(I(max)))).AsInt(), min + 1);
}

TEST(Expression, DoubleOverflowIsAnError) {
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(D(1e300)), C(D(1e300)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::SUB, C(D(-1e308)), C(D(1e308)))), DbError);
    EXPECT_DOUBLE_EQ(Eval(Bin(BinOp::MUL, C(D(1e150)), C(D(1e150)))).AsDouble(), 1e300);
}

// ---- scalar functions, CASE, || ----

ExprPtr F(const std::string& name, std::vector<ExprPtr> args) { return Expr::Func(name, std::move(args)); }

TEST(Expression, StringFunctions) {
    EXPECT_EQ(Eval(F("upper", {C(T("aBc"))})).AsText(), "ABC");
    EXPECT_EQ(Eval(F("lower", {C(T("aBc"))})).AsText(), "abc");
    EXPECT_EQ(Eval(F("length", {C(T("hello"))})).AsInt(), 5);
    EXPECT_EQ(Eval(F("length", {C(T(""))})).AsInt(), 0);
    EXPECT_THROW(Eval(F("upper", {C(I(1))})), DbError);
}

TEST(Expression, SubstrFollowsSqlSemantics) {
    auto sub = [](const std::string& s, int64_t start, std::optional<int64_t> len = std::nullopt) {
        std::vector<ExprPtr> args = {C(T(s)), C(I(start))};
        if (len) args.push_back(C(I(*len)));
        return Eval(F("substr", args)).AsText();
    };
    EXPECT_EQ(sub("hello", 2), "ello");
    EXPECT_EQ(sub("hello", 2, 3), "ell");
    EXPECT_EQ(sub("hello", 0, 2), "h") << "positions before the string count towards the length";
    EXPECT_EQ(sub("hello", -5, 3), "");
    EXPECT_EQ(sub("hello", 4, 100), "lo");
    EXPECT_EQ(sub("hello", 9), "");
    EXPECT_EQ(sub("hello", 1, INT64_MAX), "hello") << "no overflow on start + len";
    EXPECT_THROW(sub("hello", 1, -1), DbError);
}

TEST(Expression, NumericFunctions) {
    EXPECT_EQ(Eval(F("abs", {C(I(-7))})).AsInt(), 7);
    EXPECT_DOUBLE_EQ(Eval(F("abs", {C(D(-2.5))})).AsDouble(), 2.5);
    EXPECT_THROW(Eval(F("abs", {C(I(INT64_MIN))})), DbError) << "|INT64_MIN| does not fit";
    EXPECT_DOUBLE_EQ(Eval(F("round", {C(D(2.5))})).AsDouble(), 3.0) << "half away from zero";
    EXPECT_DOUBLE_EQ(Eval(F("round", {C(D(-2.5))})).AsDouble(), -3.0);
    EXPECT_DOUBLE_EQ(Eval(F("round", {C(D(3.14159)), C(I(2))})).AsDouble(), 3.14);
    EXPECT_EQ(Eval(F("round", {C(I(1234)), C(I(-2))})).AsInt(), 1200) << "INT stays INT";
    EXPECT_EQ(Eval(F("round", {C(I(17))})).Type(), TypeId::INT);
    EXPECT_THROW(Eval(F("round", {C(D(1.0)), C(I(99))})), DbError);
}

TEST(Expression, FunctionsPropagateNull) {
    for (const char* fn : {"upper", "lower", "length", "abs", "round"}) EXPECT_TRUE(Eval(F(fn, {C(N())})).IsNull()) << fn;
    EXPECT_TRUE(Eval(F("substr", {C(T("abc")), C(N())})).IsNull());
}

TEST(Expression, CoalesceAndNullif) {
    EXPECT_EQ(Eval(F("coalesce", {C(N()), C(N()), C(I(3)), C(I(4))})).AsInt(), 3);
    EXPECT_TRUE(Eval(F("coalesce", {C(N())})).IsNull());
    EXPECT_EQ(Eval(F("coalesce", {C(I(1)), Bin(BinOp::DIV, C(I(1)), C(I(0)))})).AsInt(), 1)
        << "later arguments are not evaluated once a value is found";
    EXPECT_TRUE(Eval(F("nullif", {C(I(5)), C(D(5.0))})).IsNull());
    EXPECT_EQ(Eval(F("nullif", {C(I(5)), C(I(6))})).AsInt(), 5);
    EXPECT_EQ(Eval(F("nullif", {C(I(5)), C(N())})).AsInt(), 5);
}

TEST(Expression, CaseIsLazyAndUsesThreeValuedLogic) {
    auto when = [](ExprPtr cond, ExprPtr then) { return std::vector<ExprPtr>{cond, then}; };
    auto args = when(C(B(false)), C(T("a")));
    auto more = when(C(N()), C(T("b")));  // unknown is not true
    args.insert(args.end(), more.begin(), more.end());
    args.push_back(C(T("else")));
    EXPECT_EQ(Eval(F("case", args)).AsText(), "else");
    EXPECT_TRUE(Eval(F("case", when(C(B(false)), C(I(1))))).IsNull()) << "no ELSE -> NULL";
    EXPECT_EQ(Eval(F("case", {C(B(true)), C(I(1)), Bin(BinOp::DIV, C(I(1)), C(I(0)))})).AsInt(), 1)
        << "branches not taken are not evaluated";
    EXPECT_THROW(Eval(F("case", when(C(I(1)), C(I(2))))), DbError) << "WHEN must be boolean";
}

TEST(Expression, ConcatOperator) {
    EXPECT_EQ(Eval(Bin(BinOp::CONCAT, C(T("ab")), C(T("cd")))).AsText(), "abcd");
    EXPECT_EQ(Eval(Bin(BinOp::CONCAT, C(T("n=")), C(I(5)))).AsText(), "n=5") << "non-text operands print as text";
    EXPECT_TRUE(Eval(Bin(BinOp::CONCAT, C(T("a")), C(N()))).IsNull());
}

TEST(Expression, FunctionArgumentsAreBoundAndCountTowardsHeight) {
    Schema s({Col("x", TypeId::INT)});
    auto e = F("abs", {Bin(BinOp::SUB, Expr::Col("", "x"), C(I(10)))});
    EXPECT_EQ(e->height, 3);
    EXPECT_EQ(Evaluate(*BindExpr(e, s), {I(4)}).AsInt(), 6);
    EXPECT_THROW(BindExpr(F("abs", {Expr::Col("", "nope")}), s), DbError);
    EXPECT_TRUE(ContainsAggregate(*F("coalesce", {Expr::Agg(AggFunc::MAX, Expr::Col("", "x"), false)})));
    EXPECT_EQ(F("case", {C(B(true)), C(I(1)), C(I(2))})->ToString(), "CASE WHEN true THEN 1 ELSE 2 END");
}

// The planner turns `x [NOT] IN (SELECT ...)` into an IN_SET node; build one by hand.
ExprPtr InSet(ExprPtr x, std::vector<Value> values, bool negated) {
    auto set = std::make_shared<std::unordered_set<std::string>>();
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::IN_SET;
    e->negated = negated;
    e->left = std::move(x);
    for (const auto& v : values) {
        if (v.IsNull()) {
            e->in_set_has_null = true;
            continue;
        }
        std::string k;
        v.AppendKey(&k);
        set->insert(k);
    }
    e->in_set = set;
    e->FixHeight();
    return e;
}

TEST(Expression, InSetUsesThreeValuedLogic) {
    EXPECT_TRUE(Eval(InSet(C(I(2)), {I(1), I(2)}, false)).AsBool());
    EXPECT_FALSE(Eval(InSet(C(I(3)), {I(1), I(2)}, false)).AsBool());
    EXPECT_TRUE(Eval(InSet(C(D(2.0)), {I(1), I(2)}, false)).AsBool()) << "2.0 = 2";
    EXPECT_TRUE(Eval(InSet(C(I(3)), {I(1), N()}, false)).IsNull()) << "no match, but a NULL: unknown";
    EXPECT_TRUE(Eval(InSet(C(I(1)), {I(1), N()}, false)).AsBool()) << "a match beats the NULL";
    EXPECT_TRUE(Eval(InSet(C(N()), {I(1)}, false)).IsNull());
    EXPECT_FALSE(Eval(InSet(C(N()), {}, false)).AsBool()) << "nothing is IN an empty set, not even NULL";
    // NOT IN
    EXPECT_TRUE(Eval(InSet(C(I(3)), {I(1), I(2)}, true)).AsBool());
    EXPECT_FALSE(Eval(InSet(C(I(1)), {I(1), I(2)}, true)).AsBool());
    EXPECT_TRUE(Eval(InSet(C(I(3)), {I(1), N()}, true)).IsNull()) << "the classic NOT IN + NULL trap";
    EXPECT_TRUE(Eval(InSet(C(N()), {}, true)).AsBool());
}

TEST(Expression, UnresolvedSubqueriesAreRejected) {
    auto sub = Expr::Subquery(SubqueryKind::SCALAR, nullptr);
    EXPECT_THROW(BindExpr(sub, Schema{}), DbError);
    EXPECT_EQ(Expr::Subquery(SubqueryKind::IN, nullptr, C(I(1)), true)->ToString(), "1 NOT IN (SELECT ...)");
}

TEST(Expression, FilterResultMustBeBoolean) {
    EXPECT_TRUE(IsTrue(B(true)));
    EXPECT_FALSE(IsTrue(B(false)));
    EXPECT_FALSE(IsTrue(N())) << "unknown does not pass a filter";
    EXPECT_THROW(IsTrue(I(1)), DbError) << "WHERE 1 is an error, not a silent false";
    EXPECT_THROW(IsTrue(D(0.5)), DbError);
    EXPECT_THROW(IsTrue(T("true")), DbError);
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

TEST(Expression, TreeHeightIsTrackedAndCapped) {
    EXPECT_EQ(C(I(1))->height, 1);
    auto e = Bin(BinOp::ADD, C(I(1)), Bin(BinOp::MUL, C(I(2)), C(I(3))));
    EXPECT_EQ(e->height, 3);
    EXPECT_EQ(Expr::Unary(UnaryOp::NEG, e)->height, 4);
    EXPECT_EQ(Expr::IsNull(e, false)->height, 4);

    // Building iteratively (as the parser does for a + b + c ...) stops at the limit,
    // so no pass over an expression can ever recurse deeper than kMaxExprHeight.
    ExprPtr chain = C(I(0));
    for (int i = 1; i < kMaxExprHeight; ++i) chain = Bin(BinOp::ADD, chain, C(I(1)));
    EXPECT_EQ(chain->height, kMaxExprHeight);
    EXPECT_EQ(Eval(chain).AsInt(), kMaxExprHeight - 1) << "evaluating a tree at the limit is safe";
    EXPECT_THROW(Bin(BinOp::ADD, chain, C(I(1))), DbError);
}

// ---- DATE / TIMESTAMP ----

Value Dt(const char* s) { return Value::Date(ParseDate(s)); }
Value Ts(const char* s) { return Value::Timestamp(ParseTimestamp(s)); }
Value Fn(const char* name, std::vector<ExprPtr> args) { return Eval(Expr::Func(name, std::move(args))); }

TEST(Expression, DateArithmetic) {
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(Dt("2024-02-28")), C(I(1)))).ToString(), "2024-02-29");
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(I(2)), C(Dt("2023-12-31")))).ToString(), "2024-01-02");
    EXPECT_EQ(Eval(Bin(BinOp::SUB, C(Dt("2024-03-01")), C(I(1)))).ToString(), "2024-02-29");
    Value diff = Eval(Bin(BinOp::SUB, C(Dt("2025-01-01")), C(Dt("2024-01-01"))));
    EXPECT_EQ(diff.Type(), TypeId::INT);
    EXPECT_EQ(diff.AsInt(), 366);
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(Dt("2024-01-01")), C(N()))).IsNull(), true);
}

TEST(Expression, TimestampArithmetic) {
    EXPECT_EQ(Eval(Bin(BinOp::ADD, C(Ts("2024-01-01 23:59:30")), C(I(45)))).ToString(), "2024-01-02 00:00:15");
    EXPECT_EQ(Eval(Bin(BinOp::SUB, C(Ts("2024-01-02 00:00:00")), C(Dt("2024-01-01")))).AsInt(), 86400);
    EXPECT_EQ(Eval(Bin(BinOp::SUB, C(Ts("2024-01-01 00:00:00")), C(Ts("2024-01-01 01:00:00")))).AsInt(), -3600);
}

TEST(Expression, InvalidTemporalArithmeticIsAnError) {
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(Dt("2024-01-01")), C(Dt("2024-01-01")))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::MUL, C(Dt("2024-01-01")), C(I(2)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::SUB, C(I(2)), C(Dt("2024-01-01")))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(Dt("2024-01-01")), C(D(1.5)))), DbError);
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(Dt("9999-12-31")), C(I(1)))), DbError) << "past year 9999";
    EXPECT_THROW(Eval(Bin(BinOp::ADD, C(Dt("2024-01-01")), C(I(INT64_MAX)))), DbError) << "overflow";
}

TEST(Expression, DateParts) {
    std::vector<std::pair<const char*, int64_t>> want = {{"year", 2024}, {"month", 2},   {"day", 29},
                                                         {"hour", 13},   {"minute", 5}, {"second", 9}};
    for (auto& [fn, v] : want) EXPECT_EQ(Fn(fn, {C(Ts("2024-02-29 13:05:09"))}).AsInt(), v) << fn;
    EXPECT_EQ(Fn("hour", {C(Dt("2024-02-29"))}).AsInt(), 0);
    EXPECT_EQ(Fn("year", {C(Ts("1969-12-31 23:00:00"))}).AsInt(), 1969);
    EXPECT_EQ(Fn("hour", {C(Ts("1969-12-31 23:00:00"))}).AsInt(), 23);
    EXPECT_TRUE(Fn("year", {C(N())}).IsNull());
    EXPECT_THROW(Fn("year", {C(T("2024-01-01"))}), DbError);
}

TEST(Expression, DateFunction) {
    EXPECT_EQ(Fn("date", {C(Ts("2024-02-29 13:05:09"))}).ToString(), "2024-02-29");
    EXPECT_EQ(Fn("date", {C(T("2024-02-29"))}).Type(), TypeId::DATE);
    EXPECT_THROW(Fn("date", {C(I(1))}), DbError);
}

ExprPtr Cast(ExprPtr x, TypeId t) { return Expr::Func("cast", {std::move(x), C(I(static_cast<int64_t>(t)))}); }

TEST(Expression, Cast) {
    EXPECT_EQ(Eval(Cast(C(T(" 42 ")), TypeId::INT)).AsInt(), 42);
    EXPECT_DOUBLE_EQ(Eval(Cast(C(T("2.5")), TypeId::DOUBLE)).AsDouble(), 2.5);
    EXPECT_EQ(Eval(Cast(C(T("TRUE")), TypeId::BOOL)).AsBool(), true);
    EXPECT_EQ(Eval(Cast(C(I(0)), TypeId::BOOL)).AsBool(), false);
    EXPECT_EQ(Eval(Cast(C(B(true)), TypeId::INT)).AsInt(), 1);
    EXPECT_EQ(Eval(Cast(C(D(3.9)), TypeId::INT)).AsInt(), 3);
    EXPECT_EQ(Eval(Cast(C(I(7)), TypeId::TEXT)).AsText(), "7");
    EXPECT_EQ(Eval(Cast(C(Dt("2024-01-02")), TypeId::TEXT)).AsText(), "2024-01-02");
    EXPECT_EQ(Eval(Cast(C(T("2024-01-02")), TypeId::TIMESTAMP)).ToString(), "2024-01-02 00:00:00");
    EXPECT_EQ(Eval(Cast(C(Ts("2024-01-02 10:00:00")), TypeId::DATE)).ToString(), "2024-01-02");
    EXPECT_TRUE(Eval(Cast(C(N()), TypeId::INT)).IsNull());
    for (const char* bad : {"4x", "", "99999999999999999999", "1.5"})
        EXPECT_THROW(Eval(Cast(C(T(bad)), TypeId::INT)), DbError) << bad;
    EXPECT_THROW(Eval(Cast(C(T("nope")), TypeId::BOOL)), DbError);
    EXPECT_THROW(Eval(Cast(C(I(1)), TypeId::DATE)), DbError);
    EXPECT_THROW(Eval(Cast(C(Dt("2024-01-01")), TypeId::INT)), DbError);
    EXPECT_EQ(Cast(C(T("1")), TypeId::INT)->ToString(), "CAST('1' AS INT)");
}

TEST(Expression, TemporalConstantsPrintAsLiterals) {
    EXPECT_EQ(C(Dt("2024-01-02"))->ToString(), "DATE '2024-01-02'");
    EXPECT_EQ(C(Ts("2024-01-02 03:04:05"))->ToString(), "TIMESTAMP '2024-01-02 03:04:05'");
    EXPECT_EQ(C(T("it's"))->ToString(), "'it''s'");
}

}  // namespace
}  // namespace minidb
