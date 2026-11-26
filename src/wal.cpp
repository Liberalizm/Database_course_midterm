#include "wal.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <vector>

namespace minidb {

namespace {

constexpr char kMagic[8] = {'M', 'D', 'B', 'W', 'A', 'L', '0', '1'};
constexpr uint32_t kVersion = 2;  // 2: the header records the base page count

// 64-bit FNV-1a. `h` continues a previous hash, so data in pieces can be hashed.
uint64_t Fnv1a(const char* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Record header layout (32 bytes); the checksum covers bytes [0, 24) and the payload.
struct RecordHeader {
    uint32_t type;
    page_id_t pid;
    uint32_t salt;
    uint32_t txn;
    page_id_t num_pages;  // COMMIT only
    uint32_t digest;      // COMMIT only: GroupDigest of the group's PAGE records
    uint64_t checksum;
};

void EncodeRecordHeader(char* h, const RecordHeader& r) {
    std::memset(h, 0, WriteAheadLog::kRecordHeaderSize);
    WriteAs<uint32_t>(h + 0, r.type);
    WriteAs<page_id_t>(h + 4, r.pid);
    WriteAs<uint32_t>(h + 8, r.salt);
    WriteAs<uint32_t>(h + 12, r.txn);
    WriteAs<page_id_t>(h + 16, r.num_pages);
    WriteAs<uint32_t>(h + 20, r.digest);
    WriteAs<uint64_t>(h + 24, r.checksum);
}

RecordHeader DecodeRecordHeader(const char* h) {
    return {ReadAs<uint32_t>(h + 0),  ReadAs<page_id_t>(h + 4),  ReadAs<uint32_t>(h + 8), ReadAs<uint32_t>(h + 12),
            ReadAs<page_id_t>(h + 16), ReadAs<uint32_t>(h + 20), ReadAs<uint64_t>(h + 24)};
}

uint64_t RecordChecksum(const char* header, const char* payload, size_t payload_size) {
    uint64_t h = Fnv1a(header, 24);
    return payload ? Fnv1a(payload, payload_size, h) : h;
}

// A group's digest is the sum of Mix(record) over its PAGE records; the sum
// does not depend on their order, and a record overwritten in place can be
// subtracted out again.
uint64_t Mix(page_id_t pid, uint64_t checksum) {
    uint64_t z = checksum ^ (static_cast<uint64_t>(static_cast<uint32_t>(pid)) * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;  // splitmix64 finalizer
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint32_t GroupDigest(uint64_t sum) { return static_cast<uint32_t>(sum ^ (sum >> 32)); }

}  // namespace

WriteAheadLog::WriteAheadLog(const std::string& path, bool sync, page_id_t base_pages)
    : path_(path), sync_(sync), base_pages_(base_pages) {
    if (file_.Open(path) && sync_) SyncParentDirectory(path);
    Recover();
}

void WriteAheadLog::Sync() {
    if (sync_) file_.Sync();
}

void WriteAheadLog::WriteHeader(uint32_t salt) {
    char h[kHeaderSize] = {};
    std::memcpy(h, kMagic, sizeof(kMagic));
    WriteAs<uint32_t>(h + 8, kVersion);
    WriteAs<uint32_t>(h + 12, static_cast<uint32_t>(PAGE_SIZE));
    WriteAs<uint32_t>(h + 16, salt);
    WriteAs<page_id_t>(h + 20, base_pages_);
    WriteAs<uint64_t>(h + 24, Fnv1a(h, 24));
    file_.WriteAt(0, h, kHeaderSize);
    salt_ = salt;
}

void WriteAheadLog::Recover() {
    char h[kHeaderSize];
    bool valid_header = file_.ReadAt(0, h, kHeaderSize) == kHeaderSize && std::memcmp(h, kMagic, 8) == 0 &&
                        ReadAs<uint32_t>(h + 8) == kVersion && ReadAs<uint32_t>(h + 12) == PAGE_SIZE &&
                        ReadAs<uint64_t>(h + 24) == Fnv1a(h, 24);
    const page_id_t new_base = base_pages_;
    if (valid_header) {
        salt_ = ReadAs<uint32_t>(h + 16);
        base_pages_ = ReadAs<page_id_t>(h + 20);
        // Scan the records. A group of PAGE records becomes committed when its
        // COMMIT record is read; the scan stops at the first record that is
        // incomplete, damaged, from another generation or out of sequence.
        std::unordered_map<page_id_t, uint64_t> group;
        size_t group_records = 0;
        uint64_t group_sum = 0;
        uint32_t group_txn = 0;  // 0: no group open
        uint32_t last_txn = 0;
        uint64_t off = kHeaderSize;
        std::vector<char> page(PAGE_SIZE);
        while (true) {
            char rh[kRecordHeaderSize];
            if (file_.ReadAt(off, rh, kRecordHeaderSize) != kRecordHeaderSize) break;
            RecordHeader r = DecodeRecordHeader(rh);
            if (r.salt != salt_) break;
            // Every record of a group has the group's number, and numbers only grow.
            if (group_txn == 0 ? r.txn <= last_txn : r.txn != group_txn) break;
            if (r.type == kPage) {
                if (r.pid < 0) break;
                if (file_.ReadAt(off + kRecordHeaderSize, page.data(), PAGE_SIZE) != PAGE_SIZE) break;
                if (RecordChecksum(rh, page.data(), PAGE_SIZE) != r.checksum) break;
                group_txn = r.txn;
                group[r.pid] = off + kRecordHeaderSize;
                ++group_records;
                group_sum += Mix(r.pid, r.checksum);
                off += kPageRecordSize;
            } else if (r.type == kCommit) {
                if (RecordChecksum(rh, nullptr, 0) != r.checksum || r.num_pages < 0) break;
                // The commit lists (as a digest) exactly the records it commits. A
                // mismatch means one of them is not the version that was committed:
                // a write that never reached the disk although the commit did.
                if (r.digest != GroupDigest(group_sum)) break;
                for (const auto& [pid, at] : group) committed_[pid] = at;
                group.clear();
                committed_page_records_ += group_records;
                group_records = 0;
                group_sum = 0;
                group_txn = 0;
                last_txn = r.txn;
                has_commit_ = true;
                committed_num_pages_ = r.num_pages;
                off += kRecordHeaderSize;
                committed_end_ = off;
                ++recovered_commits_;
            } else {
                break;
            }
        }
        txn_ = last_txn + 1;
    }
    if (!has_commit_) {
        // Nothing worth keeping. Start a new generation (new salt), so that no
        // record left in the file can ever be read as part of it.
        uint32_t salt = valid_header ? salt_ + 1 : static_cast<uint32_t>(std::random_device{}());
        file_.Truncate(0);
        base_pages_ = new_base;
        WriteHeader(salt);
        Sync();
        committed_end_ = kHeaderSize;
        txn_ = 1;
    } else if (file_.Size() != committed_end_) {
        file_.Truncate(committed_end_);  // drop the unfinished group / torn tail
        Sync();
    }
    end_ = committed_end_;
    page_records_ = committed_page_records_;
}

uint64_t WriteAheadLog::WriteRecord(uint64_t offset, RecordType type, page_id_t pid, page_id_t num_pages,
                                    uint32_t digest, const char* page) {
    size_t payload = page ? PAGE_SIZE : 0;
    std::vector<char> buf(kRecordHeaderSize + payload);
    if (page) std::memcpy(buf.data() + kRecordHeaderSize, page, PAGE_SIZE);
    RecordHeader r{type, pid, salt_, txn_, num_pages, digest, 0};
    EncodeRecordHeader(buf.data(), r);
    r.checksum = RecordChecksum(buf.data(), page ? buf.data() + kRecordHeaderSize : nullptr, payload);
    EncodeRecordHeader(buf.data(), r);
    file_.WriteAt(offset, buf.data(), buf.size());
    end_ = std::max<uint64_t>(end_, offset + buf.size());
    return r.checksum;
}

void WriteAheadLog::AppendPage(page_id_t pid, const char* data) {
    auto it = pending_.find(pid);
    // A record from before the innermost mark must stay as it is: rolling back to
    // the mark returns to it.
    if (it != pending_.end() && (marks_.empty() || it->second.offset - kRecordHeaderSize >= marks_.back().end)) {
        // Written before in this group (e.g. evicted twice during one statement):
        // overwrite that record, so a statement logs each page it changed once.
        // If a crash tears the record, recovery drops the uncommitted group; if
        // the overwrite is lost but the commit is not, the digest catches it.
        PendingPage& p = it->second;
        pending_sum_ -= p.mix;
        p.mix = Mix(pid, WriteRecord(p.offset - kRecordHeaderSize, kPage, pid, 0, 0, data));
        pending_sum_ += p.mix;
        return;
    }
    uint64_t at = end_ + kRecordHeaderSize;
    uint64_t mix = Mix(pid, WriteRecord(end_, kPage, pid, 0, 0, data));
    pending_[pid] = {at, mix};
    pending_sum_ += mix;
    ++page_records_;
}

void WriteAheadLog::Commit(page_id_t num_pages) {
    WriteRecord(end_, kCommit, INVALID_PAGE_ID, num_pages, GroupDigest(pending_sum_), nullptr);
    Sync();  // the moment the statement becomes durable
    for (const auto& [pid, p] : pending_) committed_[pid] = p.offset;
    pending_.clear();
    pending_sum_ = 0;
    has_commit_ = true;
    committed_num_pages_ = num_pages;
    committed_end_ = end_;
    marks_.clear();
    committed_page_records_ = page_records_;
    ++txn_;
}

void WriteAheadLog::Rollback() {
    // The state is reset first, so that even if cutting the file fails, nothing
    // of the abandoned group can be committed later (and recovery ignores it).
    pending_.clear();
    pending_sum_ = 0;
    marks_.clear();
    const uint64_t old_end = end_;
    end_ = committed_end_;
    page_records_ = committed_page_records_;
    ++txn_;  // never reuse the number of the abandoned group
    if (old_end != committed_end_) file_.Truncate(committed_end_);
}

void WriteAheadLog::SetMark() { marks_.push_back({end_, page_records_, pending_sum_, pending_}); }

void WriteAheadLog::RollbackToMark() {
    if (marks_.empty()) throw DbError("internal: no write-ahead log mark to roll back to");
    const Mark& m = marks_.back();
    pending_ = m.pending;
    pending_sum_ = m.pending_sum;
    page_records_ = m.page_records;
    const uint64_t old_end = end_;
    end_ = m.end;
    // The records cut off carry the group's transaction number. Should one of
    // them survive a crash anyway, the COMMIT record's digest rejects it.
    if (old_end != end_) file_.Truncate(end_);
}

void WriteAheadLog::ReleaseMark() {
    if (marks_.empty()) throw DbError("internal: no write-ahead log mark to release");
    marks_.pop_back();
}

bool WriteAheadLog::Find(page_id_t pid, char* out) {
    uint64_t at;
    if (auto p = pending_.find(pid); p != pending_.end()) {
        at = p->second.offset;
    } else if (auto c = committed_.find(pid); c != committed_.end()) {
        at = c->second;
    } else {
        return false;
    }
    if (file_.ReadAt(at, out, PAGE_SIZE) != PAGE_SIZE)
        throw DbError("I/O error: write-ahead log '" + path_ + "' is truncated");
    return true;
}

void WriteAheadLog::ForEachCommittedPage(const std::function<void(page_id_t, const char*)>& fn) {
    std::vector<std::pair<page_id_t, uint64_t>> pages(committed_.begin(), committed_.end());
    std::sort(pages.begin(), pages.end());  // write the database file front to back
    std::vector<char> buf(PAGE_SIZE);
    for (const auto& [pid, at] : pages) {
        if (file_.ReadAt(at, buf.data(), PAGE_SIZE) != PAGE_SIZE)
            throw DbError("I/O error: write-ahead log '" + path_ + "' is truncated");
        fn(pid, buf.data());
    }
}

void WriteAheadLog::Reset(page_id_t base_pages) {
    if (HasUncommitted()) throw DbError("internal: cannot empty the write-ahead log during a statement");
    file_.Truncate(0);
    base_pages_ = base_pages;
    WriteHeader(salt_ + 1);
    Sync();
    committed_.clear();
    has_commit_ = false;
    committed_num_pages_ = 0;
    committed_end_ = end_ = kHeaderSize;
    committed_page_records_ = page_records_ = 0;
    txn_ = 1;
}

void WriteAheadLog::CloseAndRemove() {
    file_.Close();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

}  // namespace minidb
