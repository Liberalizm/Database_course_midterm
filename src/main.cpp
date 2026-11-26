// main.cpp - interactive SQL shell for MiniDB.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "database.h"

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#define ISATTY isatty
#define FILENO fileno
#endif

using namespace minidb;

namespace {

void PrintResult(const QueryResult& r, std::ostream& os) {
    if (r.columns.empty()) {
        os << r.message << "\n";
        return;
    }
    std::vector<size_t> w(r.columns.size());
    for (size_t i = 0; i < r.columns.size(); ++i) w[i] = r.columns[i].size();
    std::vector<std::vector<std::string>> cells;
    for (const auto& row : r.rows) {
        std::vector<std::string> line;
        for (size_t i = 0; i < row.size(); ++i) {
            line.push_back(row[i].IsNull() ? "NULL" : row[i].ToString());
            w[i] = std::max(w[i], line.back().size());
        }
        cells.push_back(std::move(line));
    }
    auto print_line = [&](const std::vector<std::string>& vals) {
        for (size_t i = 0; i < vals.size(); ++i) {
            os << (i ? " | " : " ") << vals[i];
            if (i + 1 < vals.size()) os << std::string(w[i] - vals[i].size(), ' ');
        }
        os << "\n";
    };
    print_line(r.columns);
    for (size_t i = 0; i < w.size(); ++i) os << (i ? "-+-" : "-") << std::string(w[i], '-');
    os << "-\n";
    for (const auto& line : cells) print_line(line);
    if (!r.message.empty()) os << r.message << "\n";
}

void PrintHelp() {
    std::cout << R"(MiniDB - a relational database written from scratch.

SQL (end every statement with ';'):
  CREATE TABLE t (id INT PRIMARY KEY, name TEXT, score DOUBLE, ok BOOL);
  CREATE [UNIQUE] INDEX idx ON t (col);        DROP TABLE t;  DROP INDEX idx;
  INSERT INTO t VALUES (1, 'a', 2.5, true), (2, 'b', NULL, false);
  INSERT INTO t (id, name) SELECT ...;
  SELECT [DISTINCT] cols | * FROM a [JOIN b ON ...] [, c] WHERE ...
         GROUP BY ... HAVING ... ORDER BY ... [DESC] LIMIT n OFFSET m;
  UPDATE t SET col = expr WHERE ...;           DELETE FROM t WHERE ...;
  EXPLAIN [ANALYZE] SELECT ...;
  Operators: = <> < <= > >= AND OR NOT + - * / % LIKE BETWEEN IN IS [NOT] NULL
  Aggregates: COUNT(*) COUNT(x) SUM AVG MIN MAX

Meta commands:
  .tables               list tables          .schema <table>    show columns and indexes
  .indexes              list indexes         .stats             buffer pool / disk statistics
  .populate <t> <n>     insert n random rows .read <file>       run SQL from a file
  .timer on|off         show query time      .checkpoint        flush everything to disk
  .help                 this text            .exit / .quit      leave
)";
}

class Shell {
public:
    explicit Shell(Database& db) : db_(db) {}

    void RunSql(const std::string& sql) {
        auto t0 = std::chrono::steady_clock::now();
        try {
            for (const auto& r : db_.Execute(sql)) PrintResult(r, std::cout);
        } catch (const DbError& e) {
            std::cout << "ERROR: " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cout << "INTERNAL ERROR: " << e.what() << "\n";
        }
        if (timer_) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("Time: %.3f ms\n", ms);
        }
    }

    // Returns false when the shell should exit.
    bool RunMeta(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        try {
            if (cmd == ".exit" || cmd == ".quit") return false;
            if (cmd == ".help") {
                PrintHelp();
            } else if (cmd == ".tables") {
                for (TableInfo* t : db_.GetCatalog().Tables())
                    std::cout << "  " << t->name << "  (" << t->row_count << " rows, " << t->heap->PageCount()
                              << " pages)\n";
            } else if (cmd == ".indexes") {
                for (IndexInfo* i : db_.GetCatalog().Indexes())
                    std::cout << "  " << i->name << " ON " << i->table << "(" << i->column << ")"
                              << (i->unique ? " UNIQUE" : "") << "  height=" << i->tree->Height() << "\n";
            } else if (cmd == ".schema") {
                std::string name;
                in >> name;
                TableInfo* t = db_.GetCatalog().RequireTable(name);
                std::cout << "Table \"" << t->name << "\"\n";
                for (const auto& c : t->schema.Columns())
                    std::cout << "  " << c.name << "  " << TypeName(c.type) << (c.not_null ? "  NOT NULL" : "")
                              << "\n";
                for (IndexInfo* i : db_.GetCatalog().IndexesOf(name))
                    std::cout << "  index " << i->name << " (" << i->column << ")" << (i->unique ? " UNIQUE" : "")
                              << " btree\n";
            } else if (cmd == ".stats") {
                auto& b = db_.Bpm();
                auto& d = db_.Disk();
                uint64_t total = b.Hits() + b.Misses();
                std::printf("  buffer pool: %zu frames (%zu KB), %zu pinned\n", b.PoolSize(),
                            b.PoolSize() * PAGE_SIZE / 1024, b.PinnedFrames());
                std::printf("  hits=%llu misses=%llu hit-ratio=%.2f%% evictions=%llu\n",
                            static_cast<unsigned long long>(b.Hits()), static_cast<unsigned long long>(b.Misses()),
                            total ? 100.0 * static_cast<double>(b.Hits()) / static_cast<double>(total) : 0.0,
                            static_cast<unsigned long long>(b.Evictions()));
                std::printf("  disk: file=%s pages=%d reads=%llu writes=%llu\n", d.Path().c_str(), d.NumPages(),
                            static_cast<unsigned long long>(d.Reads()), static_cast<unsigned long long>(d.Writes()));
            } else if (cmd == ".populate") {
                std::string name;
                int64_t n = 0;
                in >> name >> n;
                if (name.empty() || n <= 0) throw DbError("usage: .populate <table> <rows>");
                PrintResult(db_.Populate(name, n), std::cout);
            } else if (cmd == ".read") {
                std::string path;
                in >> path;
                std::ifstream f(path);
                if (!f) throw DbError("cannot open file " + path);
                std::stringstream ss;
                ss << f.rdbuf();
                RunSql(ss.str());
            } else if (cmd == ".timer") {
                std::string v;
                in >> v;
                timer_ = (v == "on");
            } else if (cmd == ".checkpoint") {
                db_.Checkpoint();
                std::cout << "CHECKPOINT\n";
            } else {
                std::cout << "unknown command " << cmd << " (try .help)\n";
            }
        } catch (const DbError& e) {
            std::cout << "ERROR: " << e.what() << "\n";
        }
        return true;
    }

private:
    Database& db_;
    bool timer_ = false;
};

// A statement is complete when it ends with ';' outside of a string literal.
bool StatementComplete(const std::string& buf) {
    bool in_str = false;
    char last = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        char c = buf[i];
        if (c == '\'') in_str = !in_str;
        if (!in_str && c == '-' && i + 1 < buf.size() && buf[i + 1] == '-') {
            while (i < buf.size() && buf[i] != '\n') ++i;
            continue;
        }
        if (!in_str && !std::isspace(static_cast<unsigned char>(c))) last = c;
    }
    return !in_str && last == ';';
}

// True if the buffer holds anything besides whitespace and -- comments.
bool HasSqlContent(const std::string& buf) {
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == '-' && i + 1 < buf.size() && buf[i + 1] == '-') {
            while (i < buf.size() && buf[i] != '\n') ++i;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(buf[i]))) return true;
    }
    return false;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = "minidb.db";
    size_t frames = 256;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pool" && i + 1 < argc)
            frames = static_cast<size_t>(std::stoul(argv[++i]));
        else
            path = a;
    }

    try {
        Database db(path, frames);
        Shell shell(db);
        bool interactive = ISATTY(FILENO(stdin));
        if (interactive) {
            std::cout << "MiniDB 1.0 - database file '" << path << "', buffer pool " << frames
                      << " frames. Type .help for help.\n";
        }
        std::string buffer, line;
        while (true) {
            if (interactive) std::cout << (buffer.empty() ? "minidb> " : "   ...> ") << std::flush;
            if (!std::getline(std::cin, line)) break;
            // Meta commands are recognized at the start of a statement, even after comment lines.
            if (!HasSqlContent(buffer) && Trim(line).rfind('.', 0) == 0) {
                buffer.clear();
                if (!shell.RunMeta(Trim(line))) break;
                continue;
            }
            buffer += line + "\n";
            if (!HasSqlContent(buffer)) {  // only blank lines / comments so far
                buffer.clear();
                continue;
            }
            if (StatementComplete(buffer)) {
                shell.RunSql(buffer);
                buffer.clear();
            }
        }
        if (HasSqlContent(buffer)) shell.RunSql(buffer);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
