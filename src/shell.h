// shell.h - the interactive SQL shell (statement splitting, meta commands, output).
// Lives in the core library, separate from main(), so it can be unit-tested.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "database.h"

namespace minidb {

// Splits `text` into complete statements, each ending with its ';'. A ';' only
// ends a statement outside of 'string literals', "quoted identifiers" and
// -- comments. Whatever follows the last complete statement is returned in
// `rest` (it may be the beginning of a statement continued on the next line).
std::vector<std::string> SplitStatements(const std::string& text, std::string* rest);

// True if `text` holds anything besides whitespace and -- comments.
bool HasSqlContent(const std::string& text);

// Renders a result the way psql does (aligned columns, then a status line).
void PrintResult(const QueryResult& r, std::ostream& os);

class Shell {
public:
    Shell(Database& db, std::ostream& out) : db_(db), out_(out) {}

    // Runs a script line by line: SQL statements (which may span lines), meta
    // commands (".tables", ...) and comments. An error is reported and the
    // script continues with the next statement. Returns false if ".exit" ran.
    // Used both for the terminal / piped stdin and for ".read <file>".
    bool RunScript(std::istream& in, bool interactive = false);

    void RunSql(const std::string& sql);
    // Returns false when the shell should exit.
    bool RunMeta(const std::string& line);

    static constexpr int kMaxReadDepth = 16;  // ".read" of a file that reads itself

private:
    Database& db_;
    std::ostream& out_;
    bool timer_ = false;
    int read_depth_ = 0;
};

}  // namespace minidb
