// disk_manager.h - the lowest layer: reads and writes fixed-size pages in one file.
#pragma once

#include <fstream>
#include <string>

#include "common.h"

namespace minidb {

// The database is a single file, viewed as an array of PAGE_SIZE blocks.
// Page N lives at byte offset N * PAGE_SIZE.
class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void ReadPage(page_id_t pid, char* out);
    void WritePage(page_id_t pid, const char* data);

    // Extends the file by one zeroed page and returns its id.
    page_id_t AllocatePage();

    page_id_t NumPages() const { return num_pages_; }
    const std::string& Path() const { return path_; }

    uint64_t Reads() const { return reads_; }
    uint64_t Writes() const { return writes_; }

private:
    std::string path_;
    std::fstream file_;
    page_id_t num_pages_ = 0;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
};

}  // namespace minidb
