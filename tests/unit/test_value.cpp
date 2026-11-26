// Unit tests: Value, Schema and tuple (de)serialization.
#include <limits>

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

TEST(Value, DoubleToIntCastIsRangeChecked) {
    EXPECT_EQ(D(-3.7).CastTo(TypeId::INT).AsInt(), -3) << "truncates toward zero";
    EXPECT_EQ(D(-9223372036854775808.0).CastTo(TypeId::INT).AsInt(), INT64_MIN) << "-2^63 fits exactly";
    EXPECT_EQ(D(9223372036854774784.0).CastTo(TypeId::INT).AsInt(), 9223372036854774784) << "largest double < 2^63";
    EXPECT_THROW(D(9223372036854775808.0).CastTo(TypeId::INT), DbError) << "2^63 does not fit";
    EXPECT_THROW(D(-1e19).CastTo(TypeId::INT), DbError);
    EXPECT_THROW(D(std::numeric_limits<double>::infinity()).CastTo(TypeId::INT), DbError);
    EXPECT_THROW(D(std::numeric_limits<double>::quiet_NaN()).CastTo(TypeId::INT), DbError);
    EXPECT_THROW(D(1e19).AsInt(), DbError);
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

// ---- DATE / TIMESTAMP ----

TEST(Value, CalendarRoundTrip) {
    EXPECT_EQ(DaysFromCivil(1970, 1, 1), 0);
    EXPECT_EQ(DaysFromCivil(2000, 3, 1), 11017);
    EXPECT_EQ(DaysFromCivil(1969, 12, 31), -1);
    // Every day of four centuries (leap years, century rules) survives the round trip.
    for (int64_t z = DaysFromCivil(1600, 1, 1); z <= DaysFromCivil(2400, 12, 31); z += 1) {
        int64_t y;
        unsigned m, d;
        CivilFromDays(z, &y, &m, &d);
        ASSERT_EQ(DaysFromCivil(y, m, d), z);
    }
}

TEST(Value, ParseAndFormatDates) {
    EXPECT_EQ(Value::Date(ParseDate("2024-02-29")).ToString(), "2024-02-29");
    EXPECT_EQ(Value::Date(ParseDate("0001-01-01")).ToString(), "0001-01-01");
    EXPECT_EQ(Value::Date(ParseDate("9999-12-31")).ToString(), "9999-12-31");
    EXPECT_EQ(Value::Date(ParseDate("1900-02-28")).ToSqlLiteral(), "DATE '1900-02-28'");
    for (const char* bad : {"2023-02-29", "1900-02-29", "2024-13-01", "2024-00-10", "2024-01-32", "0000-01-01",
                            "24-01-01", "2024-1-1", "2024-01-01 ", "", "2024/01/01", "abcd-ef-gh"})
        EXPECT_THROW(ParseDate(bad), DbError) << bad;
}

TEST(Value, ParseAndFormatTimestamps) {
    EXPECT_EQ(Value::Timestamp(ParseTimestamp("2024-01-31 23:59:59")).ToString(), "2024-01-31 23:59:59");
    EXPECT_EQ(Value::Timestamp(ParseTimestamp("2024-01-31T08:05")).ToString(), "2024-01-31 08:05:00");
    EXPECT_EQ(Value::Timestamp(ParseTimestamp("2024-01-31")).ToString(), "2024-01-31 00:00:00");
    EXPECT_EQ(Value::Timestamp(ParseTimestamp("1969-12-31 23:59:59")).ToString(), "1969-12-31 23:59:59")
        << "negative seconds round toward the earlier day";
    EXPECT_EQ(ParseTimestamp("1970-01-01 00:00:01"), 1);
    for (const char* bad : {"2024-01-31 24:00:00", "2024-01-31 12:60", "2024-01-31 12:00:60", "2024-01-31 12",
                            "2024-01-31  12:00", "2024-01-31 12:00:00Z"})
        EXPECT_THROW(ParseTimestamp(bad), DbError) << bad;
}

TEST(Value, TemporalRangeIsChecked) {
    EXPECT_THROW(Value::Date(DaysFromCivil(10000, 1, 1)), DbError);
    EXPECT_THROW(Value::Date(DaysFromCivil(0, 12, 31)), DbError);
    EXPECT_THROW(Value::Timestamp(DaysFromCivil(10000, 1, 1) * kSecondsPerDay), DbError);
    EXPECT_NO_THROW(Value::Timestamp(DaysFromCivil(10000, 1, 1) * kSecondsPerDay - 1));
}

TEST(Value, CompareTemporal) {
    Value d1 = Value::Date(ParseDate("2024-01-01")), d2 = Value::Date(ParseDate("2024-01-02"));
    Value ts = Value::Timestamp(ParseTimestamp("2024-01-01 12:00:00"));
    EXPECT_LT(d1.Compare(d2), 0);
    EXPECT_LT(d1.Compare(ts), 0) << "a date is its midnight";
    EXPECT_GT(d2.Compare(ts), 0);
    EXPECT_EQ(d1.Compare(Value::Timestamp(ParseTimestamp("2024-01-01"))), 0);
    EXPECT_EQ(d1.Compare(T("2024-01-01")), 0) << "text is read as a date";
    EXPECT_GT(T("2024-01-01 12:00:01").Compare(ts), 0);
    EXPECT_THROW(d1.Compare(T("yesterday")), DbError);
    EXPECT_THROW(d1.Compare(I(19723)), DbError);
}

TEST(Value, CastToTemporal) {
    EXPECT_EQ(T("2024-05-06").CastTo(TypeId::DATE).ToString(), "2024-05-06");
    EXPECT_EQ(T("2024-05-06 07:08:09").CastTo(TypeId::TIMESTAMP).ToString(), "2024-05-06 07:08:09");
    EXPECT_EQ(Value::Date(0).CastTo(TypeId::TIMESTAMP).ToString(), "1970-01-01 00:00:00");
    EXPECT_EQ(Value::Timestamp(-1).CastTo(TypeId::DATE).ToString(), "1969-12-31");
    EXPECT_THROW(T("2024-05-06 07:08:09").CastTo(TypeId::DATE), DbError);
    EXPECT_THROW(I(5).CastTo(TypeId::DATE), DbError);
    EXPECT_THROW(Value::Date(0).CastTo(TypeId::INT), DbError);
}

TEST(Value, TemporalHashKeysMatchComparison) {
    std::string a, b, c;
    Value::Date(5).AppendKey(&a);
    Value::Timestamp(5 * kSecondsPerDay).AppendKey(&b);
    I(5).AppendKey(&c);
    EXPECT_EQ(a, b) << "equal values hash alike";
    EXPECT_NE(a, c);
}

TEST(Value, TemporalTupleRoundTrip) {
    Schema s({{"d", TypeId::DATE}, {"t", TypeId::TIMESTAMP}, {"n", TypeId::DATE}});
    Row r = {Value::Date(ParseDate("2024-02-29")), Value::Timestamp(ParseTimestamp("1950-06-01 10:20:30")), N()};
    std::string bytes = SerializeTuple(s, r);
    Row back = DeserializeTuple(s, bytes.data(), bytes.size());
    ASSERT_EQ(back.size(), 3u);
    EXPECT_EQ(back[0].Type(), TypeId::DATE);
    EXPECT_EQ(back[0].ToString(), "2024-02-29");
    EXPECT_EQ(back[1].ToString(), "1950-06-01 10:20:30");
    EXPECT_TRUE(back[2].IsNull());
}

}  // namespace
}  // namespace minidb
