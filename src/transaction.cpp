#include "transaction.h"

#include <array>
#include <utility>

namespace minidb {

namespace {

// Journal file layout:
//   header  "MINIDBJ1" | page size u32 | original page count i32 | 0 u32 | CRC of the 20 bytes before u32
//   record  page id i32 | CRC of (page id, contents) u32 | contents[PAGE_SIZE]      (repeated)
// A record cut short by a crash fails its CRC and ends the journal. That is safe
// because the page it belongs to is only overwritten after the record is synced.
constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'D', 'B', 'J', '1'};
constexpr size_t kHeaderSize = 24;
constexpr size_t kRecordHeader = 8;
constexpr size_t kRecordSize = kRecordHeader + PAGE_SIZE;

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
    return table;
}
constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

uint32_t Crc32(const char* data, size_t len, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = kCrcTable[(crc ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}  // namespace

TransactionManager::TransactionManager(DiskManager* disk, bool synchronous)
    : disk_(disk), sync_(synchronous), journal_path_(JournalPath(disk->Path())) {
    Recover();
    disk_->SetListener(this);
}

TransactionManager::~TransactionManager() {
    // An unfinished transaction keeps its journal, so the next open rolls it back.
    disk_->SetListener(nullptr);
}

// ---- recovery ----

void TransactionManager::Recover() {
    if (!File::Exists(journal_path_)) return;
    {
        File journal(journal_path_, /*create=*/false);
        // An invalid header means the crash came while the journal was being
        // created, before any page of the database was touched: nothing to undo.
        recovered_ = ApplyJournal(journal);
    }
    DeleteJournal();
}

bool TransactionManager::ApplyJournal(File& journal) {
    char header[kHeaderSize];
    if (journal.ReadAt(0, header, kHeaderSize) < kHeaderSize) return false;
    if (std::memcmp(header, kMagic, sizeof kMagic) != 0 || ReadAs<uint32_t>(header + 8) != PAGE_SIZE ||
        ReadAs<uint32_t>(header + 20) != Crc32(header, 20))
        return false;
    const auto orig_pages = ReadAs<int32_t>(header + 12);
    if (orig_pages < 0 || orig_pages > disk_->NumPages())
        throw DbError("the rollback journal " + journal_path_ +
                      " does not belong to this database file; move it away to open the database");

    std::string record(kRecordSize, '\0');
    for (uint64_t off = kHeaderSize;; off += kRecordSize) {
        if (journal.ReadAt(off, record.data(), kRecordSize) < kRecordSize) break;
        const auto pid = ReadAs<int32_t>(record.data());
        const uint32_t crc = Crc32(record.data() + kRecordHeader, PAGE_SIZE, Crc32(record.data(), 4));
        if (crc != ReadAs<uint32_t>(record.data() + 4)) break;  // torn last record
        if (pid < 0 || pid >= orig_pages) throw DbError("the rollback journal " + journal_path_ + " is corrupt");
        disk_->WritePage(pid, record.data() + kRecordHeader);
    }
    disk_->Truncate(orig_pages);
    if (sync_) disk_->Sync();
    return true;
}

// ---- the journal file ----

void TransactionManager::OpenJournal() {
    journal_ = File(journal_path_, /*create=*/true);
    journal_.Truncate(0);
    char header[kHeaderSize] = {};
    std::memcpy(header, kMagic, sizeof kMagic);
    WriteAs<uint32_t>(header + 8, static_cast<uint32_t>(PAGE_SIZE));
    WriteAs<int32_t>(header + 12, orig_pages_);
    WriteAs<uint32_t>(header + 20, Crc32(header, 20));
    journal_.WriteAt(0, header, kHeaderSize);
    // The header (with the original file length) must be durable before the
    // database file changes in any way, including growing.
    if (sync_) {
        journal_.Sync();
        File::SyncDirectoryOf(journal_path_);
    }
    journal_end_ = kHeaderSize;
    journal_synced_ = true;
}

void TransactionManager::AppendToJournal(page_id_t pid, const std::string& data) {
    if (!journal_.IsOpen()) OpenJournal();
    std::string record(kRecordHeader, '\0');
    WriteAs<int32_t>(record.data(), pid);
    WriteAs<uint32_t>(record.data() + 4, Crc32(data.data(), PAGE_SIZE, Crc32(record.data(), 4)));
    record += data;
    journal_.WriteAt(journal_end_, record.data(), record.size());
    journal_end_ += record.size();
    journal_synced_ = false;
    journaled_.insert(pid);
}

void TransactionManager::SyncJournal() {
    if (journal_synced_) return;
    if (sync_) journal_.Sync();
    journal_synced_ = true;
}

void TransactionManager::DeleteJournal() {
    journal_.Close();
    if (!File::Remove(journal_path_)) {
        // Another program may hold it open (e.g. a virus scanner on Windows). A
        // leftover journal would undo this transaction at the next open, so make
        // it harmless instead: an empty journal is ignored.
        File leftover(journal_path_, /*create=*/false);
        leftover.Truncate(0);
        if (sync_) leftover.Sync();
    }
    if (sync_) File::SyncDirectoryOf(journal_path_);
    journal_end_ = 0;
    journal_synced_ = true;
}

// ---- transactions ----

void TransactionManager::Begin() {
    if (active_) throw DbError("internal: a transaction is already active");
    Reset();
    orig_pages_ = disk_->NumPages();
    active_ = true;
}

void TransactionManager::Commit() {
    if (!active_) throw DbError("internal: no active transaction to commit");
    // No journal means the transaction never touched the file: nothing to sync.
    if (journal_.IsOpen()) {
        if (sync_) disk_->Sync();
        DeleteJournal();  // the commit point
    }
    Reset();
}

void TransactionManager::Rollback() {
    if (!active_) throw DbError("internal: no active transaction to roll back");
    active_ = false;  // the pages written back below are not to be journaled again
    levels_.clear();
    if (journal_.IsOpen()) {
        if (!ApplyJournal(journal_)) throw DbError("internal: the rollback journal is unreadable");
        DeleteJournal();
    }
    Reset();
}

void TransactionManager::Reset() {
    active_ = false;
    orig_pages_ = 0;
    journaled_.clear();
    levels_.clear();
    restoring_ = false;
}

// ---- savepoints ----

void TransactionManager::Savepoint() {
    if (!active_) throw DbError("internal: savepoint outside of a transaction");
    levels_.push_back(Level{disk_->NumPages(), {}});
}

void TransactionManager::ReleaseSavepoint() {
    if (levels_.empty()) throw DbError("internal: no savepoint to release");
    Level child = std::move(levels_.back());
    levels_.pop_back();
    if (levels_.empty()) return;  // the transaction's journal already has what it needs
    // The parent must be able to restore these pages too. A page the parent
    // already captured keeps the parent's (older) image. One it has not was not
    // written between the parent's start and the child's, so the child's image
    // is also the parent's.
    Level& parent = levels_.back();
    for (auto& [pid, image] : child.images)
        if (pid < parent.start_pages) parent.images.try_emplace(pid, std::move(image));
}

void TransactionManager::RollbackToSavepoint() {
    if (levels_.empty()) throw DbError("internal: no savepoint to roll back to");
    Level& level = levels_.back();
    restoring_ = true;
    try {
        for (const auto& [pid, image] : level.images) disk_->WritePage(pid, image.data());
        disk_->Truncate(level.start_pages);
    } catch (...) {
        restoring_ = false;
        throw;
    }
    restoring_ = false;
    level.images.clear();
}

void TransactionManager::BeforeWrite(page_id_t pid, bool durable) {
    if (!active_) return;
    const bool to_journal = pid < orig_pages_ && !journaled_.count(pid);
    Level* level = levels_.empty() || restoring_ ? nullptr : &levels_.back();
    const bool to_level = level && pid < level->start_pages && !level->images.count(pid);
    if (to_journal || to_level) {
        // Nothing has written this page since the transaction / savepoint began,
        // so the file still holds exactly the contents to return to.
        std::string old(PAGE_SIZE, '\0');
        disk_->ReadPage(pid, old.data());
        if (to_journal) AppendToJournal(pid, old);
        if (to_level) level->images.emplace(pid, std::move(old));
    }
    if (durable) SyncJournal();
}

void TransactionManager::BeforeGrow() {
    if (active_ && !journal_.IsOpen()) OpenJournal();
}

}  // namespace minidb
