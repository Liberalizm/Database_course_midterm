// disk_manager.h - the lowest layer: reads and writes fixed-size pages in one file.
#pragma once

#include <functional>
#include <string>

#include "common.h"
#include "os_file.h"

namespace minidb {

// The database is a single file, viewed as an array of PAGE_SIZE blocks.
// Page N lives at byte offset N * PAGE_SIZE.
class DiskManager {
public:
    // Told about every change to the file *before* it happens, so that the
    // transaction manager can save a page's old contents to its rollback journal
    // before the page is overwritten (see transaction.h).
    class WriteListener {
    public:
        virtual ~WriteListener() = default;
        // Existing page `pid` is about to be overwritten. If `durable`, whatever
        // the listener saved must be on stable storage when this returns. A caller
        // about to write a batch announces every page with durable = false first,
        // so the listener can make them all durable at once, on the first write.
        virtual void BeforeWrite(page_id_t pid, bool durable) = 0;
        // The file is about to grow by one page.
        virtual void BeforeGrow() = 0;
    };

    explicit DiskManager(const std::string& path);

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void ReadPage(page_id_t pid, char* out);
    void WritePage(page_id_t pid, const char* data);

    // Extends the file by one zeroed page and returns its id.
    page_id_t AllocatePage();

    // Announces that `pid` will be written soon (see WriteListener).
    void PrepareWrite(page_id_t pid);
    // Shrinks the file to its first `num_pages` pages (used by rollback).
    void Truncate(page_id_t num_pages);
    // Forces everything written so far onto the storage device.
    void Sync();

    void SetListener(WriteListener* l) { listener_ = l; }

    // Testing aid: called right before page data reaches the file (after the
    // listener ran). Crash tests copy the files from here to capture exactly
    // what a crash at this moment would leave behind.
    void SetBeforeWriteHook(std::function<void(page_id_t)> hook) { before_write_hook_ = std::move(hook); }

    page_id_t NumPages() const { return num_pages_; }
    const std::string& Path() const { return path_; }

    uint64_t Reads() const { return reads_; }
    uint64_t Writes() const { return writes_; }

private:
    void RawWrite(page_id_t pid, const char* data);

    std::string path_;
    File file_;
    page_id_t num_pages_ = 0;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    WriteListener* listener_ = nullptr;
    std::function<void(page_id_t)> before_write_hook_;
};

}  // namespace minidb
