// main.cpp - entry point of the MiniDB shell. The shell itself is in shell.cpp.
#include <iostream>
#include <string>

#include "shell.h"

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

int main(int argc, char** argv) {
    std::string path = "minidb.db";
    size_t frames = 256;
    bool synchronous = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--nosync") {
            synchronous = false;  // no fsync: faster, but not safe against power loss
        } else if (a == "--pool" && i + 1 < argc) {
            std::string v = argv[++i];
            try {
                size_t used = 0;
                frames = static_cast<size_t>(std::stoul(v, &used));
                if (used != v.size()) throw std::invalid_argument(v);
            } catch (const std::exception&) {
                std::cerr << "fatal: --pool expects a number of frames, got '" << v << "'\n";
                return 2;
            }
        } else {
            path = a;
        }
    }

    try {
        DiskOptions disk_options;
        disk_options.sync = synchronous;
        Database db(path, frames, disk_options);
        if (uint64_t n = db.Disk().RecoveredCommits(); n > 0)
            std::cerr << "note: recovered " << n << " committed statement(s) from the write-ahead log\n";
        Shell shell(db, std::cout);
        bool interactive = ISATTY(FILENO(stdin));
        if (interactive) {
            std::cout << "MiniDB 1.0 - database file '" << path << "', buffer pool " << frames
                      << " frames. Type .help for help.\n";
        }
        shell.RunScript(std::cin, interactive);
        // Like psql and sqlite3: what was not committed is not kept.
        if (db.InTransaction())
            std::cout << "WARNING: the transaction still open at exit was rolled back\n";
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
