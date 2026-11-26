// file_io.h - a minimal binary file with positioned reads/writes and fsync.
//
// std::fstream cannot force data to stable storage, which a write-ahead log
// needs: a commit is only durable once its log records have reached the disk.
// This wraps the native API (Win32 handles / POSIX file descriptors) instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace minidb {

class File {
public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Opens `path` for reading and writing, creating it if it does not exist.
    // Returns true if the file was created. Throws DbError on failure.
    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const;

    // Reads up to `n` bytes at `offset`; returns fewer only at the end of the file.
    size_t ReadAt(uint64_t offset, void* buf, size_t n);
    // Writes all `n` bytes at `offset`, extending the file if needed.
    void WriteAt(uint64_t offset, const void* buf, size_t n);
    uint64_t Size();
    // Shrinks or extends (with zeros) the file to exactly `size` bytes.
    void Truncate(uint64_t size);
    // Forces everything written so far to stable storage (fsync / FlushFileBuffers).
    void Sync();

    const std::string& Path() const { return path_; }

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    std::string path_;
};

// Makes the creation of a file in its directory durable (POSIX: fsync of the
// directory). A no-op on Windows, where NTFS journals directory changes.
void SyncParentDirectory(const std::string& path);

}  // namespace minidb
