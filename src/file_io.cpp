#include "file_io.h"

#include <algorithm>
#include <filesystem>

#include "common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace minidb {

namespace {

[[noreturn]] void Fail(const std::string& what, const std::string& path) {
#ifdef _WIN32
    throw DbError("I/O error: " + what + " '" + path + "' (Windows error " + std::to_string(GetLastError()) + ")");
#else
    throw DbError("I/O error: " + what + " '" + path + "' (" + std::strerror(errno) + ")");
#endif
}

}  // namespace

File::~File() {
    try {
        Close();
    } catch (...) {
    }
}

#ifdef _WIN32

bool File::Open(const std::string& path) {
    Close();
    path_ = path;
    std::wstring wpath = std::filesystem::path(path).wstring();
    // FILE_SHARE_* lets other handles read the file while it is open (tests copy
    // a live database to simulate a crash) and delete it.
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) Fail("cannot open", path);
    bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    handle_ = h;
    return created;
}

void File::Close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool File::IsOpen() const { return handle_ != nullptr; }

size_t File::ReadAt(uint64_t offset, void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        uint64_t at = offset + done;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 30));
        if (!ReadFile(static_cast<HANDLE>(handle_), static_cast<char*>(buf) + done, want, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            Fail("cannot read", path_);
        }
        if (got == 0) break;
        done += got;
    }
    return done;
}

void File::WriteAt(uint64_t offset, const void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        uint64_t at = offset + done;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD put = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 30));
        if (!WriteFile(static_cast<HANDLE>(handle_), static_cast<const char*>(buf) + done, want, &put, &ov) ||
            put == 0)
            Fail("cannot write", path_);
        done += put;
    }
}

uint64_t File::Size() {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &size)) Fail("cannot get the size of", path_);
    return static_cast<uint64_t>(size.QuadPart);
}

void File::Truncate(uint64_t size) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), pos, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(static_cast<HANDLE>(handle_)))
        Fail("cannot resize", path_);
}

void File::Sync() {
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) Fail("cannot sync", path_);
}

void SyncParentDirectory(const std::string&) {}

#else

bool File::Open(const std::string& path) {
    Close();
    path_ = path;
    bool created = false;
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        created = fd >= 0;
        if (fd < 0 && errno == EEXIST) fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);  // lost a race
    }
    if (fd < 0) Fail("cannot open", path);
    fd_ = fd;
    return created;
}

void File::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool File::IsOpen() const { return fd_ >= 0; }

size_t File::ReadAt(uint64_t offset, void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t got = ::pread(fd_, static_cast<char*>(buf) + done, n - done, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            Fail("cannot read", path_);
        }
        if (got == 0) break;
        done += static_cast<size_t>(got);
    }
    return done;
}

void File::WriteAt(uint64_t offset, const void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t put =
            ::pwrite(fd_, static_cast<const char*>(buf) + done, n - done, static_cast<off_t>(offset + done));
        if (put < 0) {
            if (errno == EINTR) continue;
            Fail("cannot write", path_);
        }
        done += static_cast<size_t>(put);
    }
}

uint64_t File::Size() {
    struct stat st;
    if (::fstat(fd_, &st) != 0) Fail("cannot get the size of", path_);
    return static_cast<uint64_t>(st.st_size);
}

void File::Truncate(uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) Fail("cannot resize", path_);
}

void File::Sync() {
    if (::fsync(fd_) != 0) Fail("cannot sync", path_);
}

void SyncParentDirectory(const std::string& path) {
    std::string dir = std::filesystem::path(path).parent_path().string();
    if (dir.empty()) dir = ".";
    int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;  // best effort
    ::fsync(fd);
    ::close(fd);
}

#endif

}  // namespace minidb
