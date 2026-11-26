// Unit tests: Value, Schema and tuple (de)serialization.
#include "test_util.h"
#include "value.h"

namespace minidb {
namespace {

using namespace testing;

TEST(Value, ToString) {
    EXPECT_EQ(N().ToString(), "NULL");
    EXPECT_EQ(I(-42).ToString(), "-42");
    EXPECT_EQ(D(2.5).ToString(), "2.5");
    EXPECT_EQ(T("hi").ToString(), "hi");
    EXPECT_EQ(B(true).ToString(), "true");
    EXPECT_EQ(B(false).ToString(), "false");
}

TEST(Value, CompareNumbersAcrossTypes) {
    EXPECT_LT(I(1).Compare(I(2)), 0);
    EXPECT_EQ(I(2).Compare(D(2.0)), 0);
    EXPECT_GT(D(2.5).Compare(I(2)), 0);
    EXPECT_GT(I(INT64_MAX).Compare(I(INT64_MAX - 1)), 0);  // no precision loss for INT/INT
}

TEST(Value, CompareTextAndBool) {
    EXPECT_LT(T("abc").Compare(T("abd")), 0);
    EXPECT_EQ(T("x").Compare(T("x")), 0);
    EXPECT_LT(B(false).Compare(B(true)), 0);
}

TEST(Value, IncompatibleComparisonThrows) {
    EXPECT_THROW(I(1).Compare(T("1")), DbError);
    EXPECT_THROW(B(true).Compare(I(1)), DbError);
}

TEST(Value, Accessors) {
    EXPECT_EQ(D(3.9).AsInt(), 3);
    EXPECT_DOUBLE_EQ(I(3).AsDouble(), 3.0);
    EXPECT_THROW(T("x").AsInt(), DbError);
    EXPECT_THROW(I(1).AsText(), DbError);
    EXPECT_THROW(I(1).AsBool(), DbError);
}

TEST(Value, CastTo) {
    EXPECT_EQ(I(3).CastTo(TypeId::DOUBLE).Type(), TypeId::DOUBLE);
    EXPECT_EQ(D(3.7).CastTo(TypeId::INT).AsInt(), 3);
    EXPECT_TRUE(N().CastTo(TypeId::INT).IsNull());
    EXPECT_THROW(T("3").CastTo(TypeId::INT), DbError);
    EXPECT_THROW(I(1).CastTo(TypeId::TEXT), DbError);
    EXPECT_THROW(B(true).CastTo(TypeId::INT), DbError);
}

TEST(Value, HashKeyTreatsIntegralDoublesAsInts) {
    auto key = [](const Value& v) {
        std::string k;
        v.AppendKey(&k);
        return k;
    };
    EXPECT_EQ(key(I(1)), key(D(1.0)));
    EXPECT_NE(key(I(1)), key(D(1.5)));
    EXPECT_NE(key(T("1")), key(I(1)));
    EXPECT_NE(key(N()), key(I(0)));
    EXPECT_NE(key(T("ab")), key(T("a")));
}

TEST(Schema, FindQualifiedUnqualifiedAndAmbiguous) {
    Schema s({Col("id", TypeId::INT, "a"), Col("name", TypeId::TEXT, "a"), Col("id", TypeId::INT, "b")});
    EXPECT_EQ(s.Find("", "name"), 1);
    EXPECT_EQ(s.Find("a", "id"), 0);
    EXPECT_EQ(s.Find("b", "id"), 2);
    EXPECT_EQ(s.Find("", "missing"), -1);
    EXPECT_EQ(s.Find("c", "id"), -1);
    EXPECT_THROW(s.Find("", "id"), DbError);
}

TEST(Schema, ConcatAndQualifier) {
    Schema a({Col("x", TypeId::INT)});
    Schema b({Col("y", TypeId::TEXT)});
    Schema c = Schema::Concat(a.WithQualifier("t1"), b.WithQualifier("t2"));
    ASSERT_EQ(c.Size(), 2u);
    EXPECT_EQ(c.At(0).table, "t1");
    EXPECT_EQ(c.At(1).table, "t2");
    EXPECT_EQ(c.Find("t2", "y"), 1);
}

TEST(Tuple, RoundTripAllTypesWithNulls) {
    Schema s({Col("i", TypeId::INT), Col("d", TypeId::DOUBLE), Col("t", TypeId::TEXT), Col("b", TypeId::BOOL),
              Col("n", TypeId::INT), Col("e", TypeId::TEXT)});
    Row in = {I(-7), D(1.25), T("hello"), B(true), N(), T("")};
    std::string bytes = SerializeTuple(s, in);
    Row out = DeserializeTuple(s, bytes.data(), bytes.size());
    ASSERT_EQ(out.size(), in.size());
    EXPECT_EQ(out[0].AsInt(), -7);
    EXPECT_DOUBLE_EQ(out[1].AsDouble(), 1.25);
    EXPECT_EQ(out[2].AsText(), "hello");
    EXPECT_TRUE(out[3].AsBool());
    EXPECT_TRUE(out[4].IsNull());
    EXPECT_EQ(out[5].AsText(), "");
}

TEST(Tuple, NullsTakeNoSpace) {
    Schema s({Col("a", TypeId::INT), Col("b", TypeId::INT)});
    EXPECT_EQ(SerializeTuple(s, {N(), N()}).size(), 1u);      // just the bitmap
    EXPECT_EQ(SerializeTuple(s, {I(1), N()}).size(), 1u + 8u);
}

TEST(Tuple, ManyColumnsUseMultiByteBitmap) {
    std::vector<Column> cols;
    Row row;
    for (int i = 0; i < 20; ++i) {
        cols.push_back(Col("c" + std::to_string(i), TypeId::INT));
        row.push_back(i % 3 == 0 ? N() : I(i));
    }
    Schema s(cols);
    std::string bytes = SerializeTuple(s, row);
    Row out = DeserializeTuple(s, bytes.data(), bytes.size());
    for (int i = 0; i < 20; ++i) {
        if (i % 3 == 0)
            EXPECT_TRUE(out[i].IsNull());
        else
            EXPECT_EQ(out[i].AsInt(), i);
    }
}

TEST(Tuple, CorruptDataIsDetected) {
    Schema s({Col("t", TypeId::TEXT)});
    std::string bytes = SerializeTuple(s, {T("a long string")});
    EXPECT_THROW(DeserializeTuple(s, bytes.data(), bytes.size() - 3), DbError);
    EXPECT_THROW(DeserializeTuple(s, bytes.data(), 0), DbError);
}

TEST(Tuple, RowSizeMismatchThrows) {
    Schema s({Col("a", TypeId::INT)});
    EXPECT_THROW(SerializeTuple(s, {I(1), I(2)}), DbError);
}

}  // namespace
}  // namespace minidb
