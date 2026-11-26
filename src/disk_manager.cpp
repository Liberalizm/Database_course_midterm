#include "disk_manager.h"

namespace minidb {

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
    num_pages_ = static_cast<page_id_t>(size / static_cast<int64_t>(PAGE_SIZE));
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void DiskManager::ReadPage(page_id_t pid, char* out) {
    if (pid < 0 || pid >= num_pages_) throw DbError("read of invalid page " + std::to_string(pid));
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pid) * static_cast<std::streamoff>(PAGE_SIZE));
    file_.read(out, PAGE_SIZE);
    auto got = file_.gcount();
    if (got < static_cast<std::streamsize>(PAGE_SIZE)) {
        std::memset(out + got, 0, PAGE_SIZE - static_cast<size_t>(got));
    }
    ++reads_;
}

void DiskManager::WritePage(page_id_t pid, const char* data) {
    if (pid < 0 || pid >= num_pages_) throw DbError("write of invalid page " + std::to_string(pid));
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(pid) * static_cast<std::streamoff>(PAGE_SIZE));
    file_.write(data, PAGE_SIZE);
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
