// os_file.h - a binary file with positioned I/O, truncation and fsync.
//
// std::fstream can neither shrink a file nor force its contents to the storage
// device, and both are needed for crash-safe transactions (see transaction.h).
// This is a thin wrapper over C stdio plus the two platform calls that differ
// between POSIX and Windows.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace minidb {

class File {
public:
    File() = default;
    // Opens `path` for reading and writing. A missing file is created if
    // `create` is true; otherwise (or if creating fails) DbError is thrown.
    File(const std::string& path, bool create);
    ~File();

    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool IsOpen() const { return f_ != nullptr; }
    const std::string& Path() const { return path_; }
    void Close();

    // Reads up to `n` bytes at `offset`; returns how many were read (fewer at
    // the end of the file).
    size_t ReadAt(uint64_t offset, char* out, size_t n);
    // Writes all `n` bytes at `offset` (extending the file if needed).
    void WriteAt(uint64_t offset, const char* data, size_t n);
    uint64_t Size();
    void Truncate(uint64_t size);
    // Returns once everything written so far is on the storage device, not just
    // in the operating system's cache (fsync / _commit).
    void Sync();

    static bool Exists(const std::string& path);
    // Deletes the file; true if it is gone afterwards (also if it never existed).
    static bool Remove(const std::string& path);
    // POSIX: syncs the directory containing `path`, so that creating or deleting
    // that file is itself durable. A no-op on Windows, where it is not needed.
    static void SyncDirectoryOf(const std::string& path);

private:
    void Seek(uint64_t offset);

    std::FILE* f_ = nullptr;
    std::string path_;
};

}  // namespace minidb
