#include "shell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace minidb {

namespace {

const char* kHelp = R"(MiniDB - a relational database written from scratch.

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
  .populate <t> <n>     insert n random rows .read <file>       run a script (SQL + meta commands)
  .timer on|off         show query time      .checkpoint        flush everything to disk
  .help                 this text            .exit / .quit      leave
)";

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

std::vector<std::string> SplitStatements(const std::string& text, std::string* rest) {
    std::vector<std::string> out;
    const size_t n = text.size();
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = text[i];
        if (c == '\'' || c == '"') {
            // Skip to the closing quote. A doubled quote ('it''s') simply closes
            // and immediately reopens a literal, which this handles naturally.
            size_t close = text.find(c, i + 1);
            if (close == std::string::npos) break;  // still open: statement continues
            i = close;
        } else if (c == '-' && i + 1 < n && text[i + 1] == '-') {
            size_t eol = text.find('\n', i);
            if (eol == std::string::npos) break;
            i = eol;
        } else if (c == ';') {
            std::string stmt = text.substr(start, i - start + 1);
            if (HasSqlContent(stmt.substr(0, stmt.size() - 1))) out.push_back(std::move(stmt));  // skip bare ";"
            start = i + 1;
        }
    }
    *rest = text.substr(start);
    return out;
}

bool HasSqlContent(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(text[i]))) return true;
    }
    return false;
}

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

bool Shell::RunScript(std::istream& in, bool interactive) {
    std::string buffer, line;
    while (true) {
        if (interactive) out_ << (HasSqlContent(buffer) ? "   ...> " : "minidb> ") << std::flush;
        if (!std::getline(in, line)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // scripts with CRLF line endings
        // Meta commands are recognized at the start of a statement, even after comment lines.
        if (!HasSqlContent(buffer) && Trim(line).rfind('.', 0) == 0) {
            buffer.clear();
            if (!RunMeta(Trim(line))) return false;
            continue;
        }
        buffer += line + "\n";
        // Each statement runs on its own, so one failing statement never takes
        // the others on the same line down with it.
        std::string rest;
        for (const auto& stmt : SplitStatements(buffer, &rest)) RunSql(stmt);
        buffer = HasSqlContent(rest) ? rest : "";
    }
    if (HasSqlContent(buffer)) RunSql(buffer);  // last statement without ';'
    return true;
}

void Shell::RunSql(const std::string& sql) {
    auto t0 = std::chrono::steady_clock::now();
    try {
        for (const auto& r : db_.Execute(sql)) PrintResult(r, out_);
    } catch (const DbError& e) {
        out_ << "ERROR: " << e.what() << "\n";
    } catch (const std::exception& e) {
        out_ << "INTERNAL ERROR: " << e.what() << "\n";
    }
    if (timer_) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char buf[64];
        std::snprintf(buf, sizeof buf, "Time: %.3f ms\n", ms);
        out_ << buf;
    }
}

bool Shell::RunMeta(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    try {
        if (cmd == ".exit" || cmd == ".quit") return false;
        if (cmd == ".help") {
            out_ << kHelp;
        } else if (cmd == ".tables") {
            for (TableInfo* t : db_.GetCatalog().Tables())
                out_ << "  " << t->name << "  (" << t->row_count << " rows, " << t->heap->PageCount() << " pages)\n";
        } else if (cmd == ".indexes") {
            for (IndexInfo* i : db_.GetCatalog().Indexes())
                out_ << "  " << i->name << " ON " << i->table << "(" << i->column << ")" << (i->unique ? " UNIQUE" : "")
                     << "  height=" << i->tree->Height() << "\n";
        } else if (cmd == ".schema") {
            std::string name;
            in >> name;
            TableInfo* t = db_.GetCatalog().RequireTable(name);
            out_ << "Table \"" << t->name << "\"\n";
            for (const auto& c : t->schema.Columns())
                out_ << "  " << c.name << "  " << TypeName(c.type) << (c.not_null ? "  NOT NULL" : "") << "\n";
            for (IndexInfo* i : db_.GetCatalog().IndexesOf(name))
                out_ << "  index " << i->name << " (" << i->column << ")" << (i->unique ? " UNIQUE" : "") << " btree\n";
        } else if (cmd == ".stats") {
            auto& b = db_.Bpm();
            auto& d = db_.Disk();
            uint64_t total = b.Hits() + b.Misses();
            char buf[512];
            std::snprintf(buf, sizeof buf, "  buffer pool: %zu frames (%zu KB), %zu pinned\n", b.PoolSize(),
                          b.PoolSize() * PAGE_SIZE / 1024, b.PinnedFrames());
            out_ << buf;
            std::snprintf(buf, sizeof buf, "  hits=%llu misses=%llu hit-ratio=%.2f%% evictions=%llu\n",
                          static_cast<unsigned long long>(b.Hits()), static_cast<unsigned long long>(b.Misses()),
                          total ? 100.0 * static_cast<double>(b.Hits()) / static_cast<double>(total) : 0.0,
                          static_cast<unsigned long long>(b.Evictions()));
            out_ << buf;
            out_ << "  disk: file=" << d.Path() << " pages=" << d.NumPages() << " reads=" << d.Reads()
                 << " writes=" << d.Writes() << " free pages=" << b.FreePages().size() << "\n";
        } else if (cmd == ".populate") {
            std::string name;
            int64_t n = 0;
            in >> name >> n;
            if (name.empty() || n <= 0) throw DbError("usage: .populate <table> <rows>");
            PrintResult(db_.Populate(name, n), out_);
        } else if (cmd == ".read") {
            std::string path;
            std::getline(in >> std::ws, path);  // the rest of the line, so paths may contain spaces
            if (path.empty()) throw DbError("usage: .read <file>");
            if (read_depth_ >= kMaxReadDepth)
                throw DbError(".read nested more than " + std::to_string(kMaxReadDepth) + " levels deep");
            std::ifstream f(path);
            if (!f) throw DbError("cannot open file " + path);
            ++read_depth_;
            bool keep_going;
            try {
                keep_going = RunScript(f);
            } catch (...) {
                --read_depth_;
                throw;
            }
            --read_depth_;
            return keep_going;  // ".exit" inside a script ends the shell, as in sqlite3
        } else if (cmd == ".timer") {
            std::string v;
            in >> v;
            if (v != "on" && v != "off") throw DbError("usage: .timer on|off");
            timer_ = (v == "on");
        } else if (cmd == ".checkpoint") {
            db_.Checkpoint();
            out_ << "CHECKPOINT\n";
        } else {
            out_ << "unknown command " << cmd << " (try .help)\n";
        }
    } catch (const DbError& e) {
        out_ << "ERROR: " << e.what() << "\n";
    }
    return true;
}

}  // namespace minidb
