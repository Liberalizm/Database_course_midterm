#include "disk_manager.h"

#include <cstdio>

#include "checksum.h"

namespace minidb {

namespace {

std::streamoff PageOffset(page_id_t pid) {
    return static_cast<std::streamoff>(pid) * static_cast<std::streamoff>(DISK_PAGE_SIZE);
}

std::string Hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(v));
    return buf;
}

}  // namespace

DiskManager::DiskManager(const std::string& path) : path_(path) {
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // File does not exist yet: create it, then reopen in read/write mode.
        std::ofstream create(path, std::ios::binary);
        create.close();
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) throw DbError("cannot open database file: " + path);
    }
    file_.seekg(0, std::ios::end);
    auto size = static_cast<int64_t>(file_.tellg());
    if (size % static_cast<int64_t>(DISK_PAGE_SIZE) != 0) {
        // Either the file is damaged (truncated, or something else entirely) or it
        // was written by an older MiniDB whose pages had no checksums.
        throw DbError("not a valid database file: " + path + " (size " + std::to_string(size) +
                      " is not a multiple of the " + std::to_string(DISK_PAGE_SIZE) + "-byte page)");
    }
    num_pages_ = static_cast<page_id_t>(size / static_cast<int64_t>(DISK_PAGE_SIZE));
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void DiskManager::ReadPage(page_id_t pid, char* out) {
    if (pid < 0 || pid >= num_pages_) throw DbError("read of invalid page " + std::to_string(pid));
    char stored[PAGE_CHECKSUM_SIZE];
    file_.clear();
    file_.seekg(PageOffset(pid));
    file_.read(out, PAGE_SIZE);
    file_.read(stored, PAGE_CHECKSUM_SIZE);
    if (!file_) {
        // The file shrank under us (we checked num_pages_ above).
        throw DbError("I/O error while reading page " + std::to_string(pid) + " (file truncated?)");
    }
    ++reads_;
    uint32_t expected = ReadAs<uint32_t>(stored);
    uint32_t actual = PageChecksum(pid, out);
    if (actual != expected) {
        throw DbError("page " + std::to_string(pid) + " failed its checksum (stored " + Hex32(expected) +
                      ", computed " + Hex32(actual) + "): the database file is damaged");
    }
}

void DiskManager::WritePage(page_id_t pid, const char* data) {
    if (pid < 0 || pid >= num_pages_) throw DbError("write of invalid page " + std::to_string(pid));
    char checksum[PAGE_CHECKSUM_SIZE];
    WriteAs<uint32_t>(checksum, PageChecksum(pid, data));
    file_.clear();
    file_.seekp(PageOffset(pid));
    file_.write(data, PAGE_SIZE);
    file_.write(checksum, PAGE_CHECKSUM_SIZE);
    file_.flush();
    if (!file_) throw DbError("I/O error while writing page " + std::to_string(pid));
    ++writes_;
}

page_id_t DiskManager::AllocatePage() {
    page_id_t pid = num_pages_++;
    char zeros[PAGE_SIZE] = {};
    WritePage(pid, zeros);
    return pid;
}

}  // namespace minidb
