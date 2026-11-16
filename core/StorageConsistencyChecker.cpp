#include "core/StorageConsistencyChecker.hpp"

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "storage/Page.hpp"
#include "wal/LogManager.hpp"
#include "wal/LogPayload.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {

StorageConsistencyReport StorageConsistencyChecker::check(
    FileManager& files, FreePageManager& freePages,
    MetadataManager& metadata, LogManager& wal) const {
  const auto recoveryContext = wal.prepareRecovery();
  RecoveryEngine recoveryEngine;
  const auto& records = recoveryContext.records();
  RecoveryContext suffix;
  std::uint64_t previousTransactions = 0;
  const auto boundary = files.recoverySnapshotBoundary();
  if (boundary) {
    for (const auto& record : records) {
      if (record.lsn().value() > *boundary) suffix.addRecord(record);
      else if (record.type() == LogRecordType::TransactionStarted) ++previousTransactions;
    }
  }
  const auto recoveryPlan = recoveryEngine.analyze(boundary ? suffix : recoveryContext);
  const auto lastLsn = records.empty() ? std::uint64_t{0} : records.back().lsn().value();
  checkPages(files, freePages, metadata, lastLsn);
  return StorageConsistencyReport{
    .pagesChecked = files.header().totalPages,
    .walRecordsChecked = records.size(),
    .transactionsChecked = previousTransactions + recoveryPlan.transactionStatuses().size(),
  };
}

void StorageConsistencyChecker::checkPages(FileManager& files, FreePageManager& freePages,
                                           MetadataManager& metadata, std::uint64_t lastLsn) const {
  const auto pageCount = files.header().totalPages;
  if (pageCount > static_cast<std::uint64_t>(std::numeric_limits<PageId>::max()) + 1) {
    throw std::runtime_error("Storage consistency failure: page count exceeds addressable page ids");
  }

  std::unordered_set<PageId> freeIds;
  for (const auto pageId : freePages.freePages()) {
    if (pageId < kFirstUserPageId || static_cast<std::uint64_t>(pageId) >= pageCount || !freeIds.insert(pageId).second) {
      throw std::runtime_error("Storage consistency failure: invalid or duplicate free-list page id");
    }
    const auto page = files.readPage(pageId);
    if (page.header().pageType != PageType::Free) {
      throw std::runtime_error("Storage consistency failure: free-list page is not marked Free");
    }
  }

  for (const auto& entry : metadata.pageDirectory().entries()) {
    if (entry.pageId < kFirstUserPageId || static_cast<std::uint64_t>(entry.pageId) >= pageCount) {
      throw std::runtime_error("Storage consistency failure: page directory entry is outside user page range");
    }
  }

  for (std::uint64_t rawId = 0; rawId < pageCount; ++rawId) {
    const auto pageId = static_cast<PageId>(rawId);
    const auto page = files.readPage(pageId); // deserialize verifies CRC
    if (page.header().pageId != pageId) {
      throw std::runtime_error("Storage consistency failure: page header id differs from physical slot");
    }
    const auto type = page.header().pageType;
    if (type < PageType::Metadata || type > PageType::Reserved ||
        (pageId <= kPageDirectoryPageId && type != PageType::Metadata) ||
        (pageId == kFreePageListPageId && type != PageType::Free)) {
      throw std::runtime_error("Storage consistency failure: invalid page type");
    }
    if (page.header().version == Page::kFormatVersionV2 && page.pageLsn() > lastLsn) {
      throw std::runtime_error("Storage consistency failure: Page LSN is beyond validated WAL");
    }
    const auto* entry = metadata.pageDirectory().get(pageId);
    if (entry != nullptr && entry->status != PageStatus::InUse && entry->status != PageStatus::Free &&
        entry->status != PageStatus::Reserved) {
      throw std::runtime_error("Storage consistency failure: page directory has an unsupported page status");
    }
    if (pageId >= kFirstUserPageId) {
      const bool isFree = freeIds.contains(pageId);
      if (metadata.isDirectoryContinuation(pageId)) {
        if (page.header().pageType != PageType::Metadata || isFree || entry != nullptr) {
          throw std::runtime_error("Storage consistency failure: invalid page directory continuation");
        }
        continue;
      }
      if (isFree != (page.header().pageType == PageType::Free)) {
        throw std::runtime_error("Storage consistency failure: page type and free-list membership disagree");
      }
      if (page.header().pageType != PageType::Free && page.header().pageType != PageType::Reserved && entry == nullptr) {
        throw std::runtime_error("Storage consistency failure: allocated page is missing from directory");
      }
      if (page.header().pageType == PageType::Reserved && entry != nullptr && entry->status != PageStatus::Reserved) {
        throw std::runtime_error("Storage consistency failure: reserved page has a non-reserved directory status");
      }
      if (entry != nullptr && (entry->pageType != page.header().pageType ||
          (entry->status == PageStatus::Free) != isFree)) {
        throw std::runtime_error("Storage consistency failure: page directory disagrees with page/free-list state");
      }
      if (entry != nullptr && ((entry->status == PageStatus::Reserved) !=
          (page.header().pageType == PageType::Reserved))) {
        throw std::runtime_error("Storage consistency failure: reserved page status/type mismatch");
      }
    }
  }

  std::unordered_set<PageId> roots;
  for (const auto& table : metadata.databaseMetadata().tables()) {
    const auto* entry = metadata.pageDirectory().get(table.rootPageId());
    if (entry == nullptr || entry->status != PageStatus::InUse || entry->pageType != PageType::Table ||
        freeIds.contains(table.rootPageId()) || entry->ownerTable != table.name() ||
        !roots.insert(table.rootPageId()).second) {
      throw std::runtime_error("Storage consistency failure: table root is missing, free, or incorrectly typed");
    }
  }

}

} // namespace zenthrildb
