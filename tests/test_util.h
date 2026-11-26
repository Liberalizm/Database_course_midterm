// Shared helpers for the MiniDB test suite.
#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "executors.h"
#include "value.h"

namespace minidb::testing {

// A unique database file path that is deleted before and after the test.
class TempFile {
public:
    TempFile() {
        // CTest runs every test in its own process, many in parallel, so the name
        // must be unique across processes, not just within one.
        static std::atomic<int> counter{0};
        static const std::string process_tag = std::to_string(std::random_device{}()) + "_" +
                                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name() : "suite";
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        path_ = (std::filesystem::temp_directory_path() /
                 ("minidb_" + name + "_" + process_tag + "_" + std::to_string(counter++) + ".db"))
                    .string();
        std::remove(path_.c_str());
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

inline Column Col(const std::string& name, TypeId type, const std::string& table = "") {
    Column c{};
    c.name = name;
    c.type = type;
    c.table = table;
    return c;
}

// An executor that produces a fixed list of rows. Lets every operator be tested
// in isolation, without tables, the buffer pool, or the planner.
class VectorExecutor : public Executor {
public:
    VectorExecutor(Schema schema, std::vector<Row> rows) : rows_(std::move(rows)) {
        schema_ = std::move(schema);
        est_rows = static_cast<double>(rows_.size());
    }
    std::string Describe() const override { return "Vector"; }

protected:
    void InitImpl() override { pos_ = 0; }
    bool NextImpl(Row* out) override {
        if (pos_ >= rows_.size()) return false;
        *out = rows_[pos_++];
        return true;
    }

private:
    std::vector<Row> rows_;
    size_t pos_ = 0;
};

inline std::vector<Row> Drain(Executor& e) {
    std::vector<Row> out;
    e.Init();
    Row r;
    while (e.Next(&r)) out.push_back(r);
    return out;
}

inline Value I(int64_t v) { return Value::Int(v); }
inline Value D(double v) { return Value::Double(v); }
inline Value T(const std::string& v) { return Value::Text(v); }
inline Value B(bool v) { return Value::Bool(v); }
inline Value N() { return Value::Null(); }

}  // namespace minidb::testing
