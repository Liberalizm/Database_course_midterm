#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS  // fopen: we check every result ourselves
#endif

#include "os_file.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>

#include "common.h"

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace minidb {

namespace {

[[noreturn]] void Fail(const std::string& what, const std::string& path) {
    throw DbError("I/O error: " + what + " " + path + " (" + std::strerror(errno) + ")");
}

}  // namespace

File::File(const std::string& path, bool create) : path_(path) {
    f_ = std::fopen(path.c_str(), "r+b");
    if (!f_ && create) f_ = std::fopen(path.c_str(), "w+b");
    if (!f_) Fail("cannot open", path);
    // Unbuffered: every write goes straight to the OS, so another process (or a
    // crash) sees exactly what was written, and Sync() has nothing to flush first.
    std::setvbuf(f_, nullptr, _IONBF, 0);
}

File::~File() { Close(); }

File::File(File&& o) noexcept : f_(std::exchange(o.f_, nullptr)), path_(std::move(o.path_)) {}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        Close();
        f_ = std::exchange(o.f_, nullptr);
        path_ = std::move(o.path_);
    }
    return *this;
}

void File::Close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

void File::Seek(uint64_t offset) {
    std::clearerr(f_);
#ifdef _WIN32
    int rc = _fseeki64(f_, static_cast<__int64>(offset), SEEK_SET);
#else
    int rc = fseeko(f_, static_cast<off_t>(offset), SEEK_SET);
#endif
    if (rc != 0) Fail("cannot seek in", path_);
}

size_t File::ReadAt(uint64_t offset, char* out, size_t n) {
    Seek(offset);
    size_t got = std::fread(out, 1, n, f_);
    if (got < n && std::ferror(f_)) Fail("cannot read", path_);
    return got;
}

void File::WriteAt(uint64_t offset, const char* data, size_t n) {
    Seek(offset);
    if (std::fwrite(data, 1, n, f_) != n || std::fflush(f_) != 0) Fail("cannot write", path_);
}

uint64_t File::Size() {
    std::clearerr(f_);
#ifdef _WIN32
    if (_fseeki64(f_, 0, SEEK_END) != 0) Fail("cannot seek in", path_);
    __int64 size = _ftelli64(f_);
#else
    if (fseeko(f_, 0, SEEK_END) != 0) Fail("cannot seek in", path_);
    off_t size = ftello(f_);
#endif
    if (size < 0) Fail("cannot get the size of", path_);
    return static_cast<uint64_t>(size);
}

void File::Truncate(uint64_t size) {
    std::fflush(f_);
#ifdef _WIN32
    bool ok = _chsize_s(_fileno(f_), static_cast<__int64>(size)) == 0;
#else
    bool ok = ftruncate(fileno(f_), static_cast<off_t>(size)) == 0;
#endif
    if (!ok) Fail("cannot truncate", path_);
}

void File::Sync() {
    if (std::fflush(f_) != 0) Fail("cannot flush", path_);
#ifdef _WIN32
    bool ok = _commit(_fileno(f_)) == 0;
#else
    bool ok = fsync(fileno(f_)) == 0;
#endif
    if (!ok) Fail("cannot sync", path_);
}

bool File::Exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool File::Remove(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !Exists(path);
}

void File::SyncDirectoryOf(const std::string& path) {
#ifndef _WIN32
    std::string dir = std::filesystem::path(path).parent_path().string();
    if (dir.empty()) dir = ".";
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;  // best effort: some file systems do not allow this
    (void)fsync(fd);
    ::close(fd);
#else
    (void)path;
#endif
}

}  // namespace minidb
