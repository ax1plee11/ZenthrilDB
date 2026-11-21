#include "transaction/StorageMutationCoordinator.hpp"

#include "core/CRCManager.hpp"
#include "core/FileManager.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {
namespace {

std::mutex coordinatorRegistryMutex;
std::unordered_set<FileManager*> coordinatorRegistry;

std::array<Byte, DatabaseHeader::serializedSize()> serializeHeader(DatabaseHeader header) {
  header.checksum = 0;
  const auto checksumInput = header.serialize();
  header.checksum = CRCManager::compute(checksumInput);
  return header.serialize();
}

Page blankBeforeImage(PageId pageId) {
  Page page;
  page.header().version = Page::kFormatVersionV2;
  page.header().pageId = pageId;
  page.header().pageType = PageType::Free;
  return page;
}

} // namespace

StorageMutationCoordinator::StorageMutationCoordinator(
    FileManager& files, LogManager& wal, TransactionManager& transactions)
  : files_(files), wal_(wal), transactions_(transactions) {
  std::lock_guard lock(coordinatorRegistryMutex);
  if (!coordinatorRegistry.insert(&files_).second) {
    throw std::logic_error("A FileManager may have only one active StorageMutationCoordinator");
  }
}

StorageMutationCoordinator::~StorageMutationCoordinator() {
  std::lock_guard lock(coordinatorRegistryMutex);
  coordinatorRegistry.erase(&files_);
}

void StorageMutationCoordinator::writePagesAtomically(
    std::vector<Page> pages, std::optional<DatabaseHeader> beforeHeader,
    std::optional<std::uint64_t> resultingPageCount) {
  auto operation = files_.operation();
  std::lock_guard lock(mutex_);
  if (pages.empty() && !beforeHeader) return;

  std::unordered_set<PageId> pageIds;
  for (auto& page : pages) {
    if (!pageIds.insert(page.header().pageId).second) {
      throw std::invalid_argument("Storage mutation contains duplicate page ids");
    }
    page.header().version = Page::kFormatVersionV2;
  }

  auto transaction = transactions_.begin();
  std::uint64_t previousTxLsn = 0;
  try {
    if (beforeHeader) {
      const auto beforeHeaderImage = serializeHeader(*beforeHeader);
      auto targetHeader = files_.header();
      if (resultingPageCount) targetHeader.totalPages = *resultingPageCount;
      const auto afterHeaderImage = serializeHeader(targetHeader);
      if (beforeHeaderImage != afterHeaderImage) {
      const auto headerLsn = wal_.appendWithLsn(LogRecordType::DatabaseHeaderImageWritten,
        [&](LogSequenceNumber) {
          return LogPayloadCodec::encodeDatabaseHeaderImageWritten(DatabaseHeaderImageWrittenPayload{
            .transactionId = transaction.id(),
            .previousTxLsn = previousTxLsn,
            .beforeImage = beforeHeaderImage,
            .afterImage = afterHeaderImage,
          });
        });
      previousTxLsn = headerLsn.value();
      files_.restoreHeaderImage(afterHeaderImage, false);
      }
    } else if (resultingPageCount && *resultingPageCount != files_.header().totalPages) {
      throw std::invalid_argument("Changing the database page count requires a before-header image");
    }

    for (auto& afterPage : pages) {
      const auto pageId = afterPage.header().pageId;
      const auto finalPageCount = resultingPageCount.value_or(files_.header().totalPages);
      if (static_cast<std::uint64_t>(pageId) >= finalPageCount) {
        throw std::invalid_argument("Storage mutation page id is outside the resulting database size");
      }
      Page beforePage;
      try { beforePage = files_.readPage(pageId); }
      catch (const PageNotFoundError&) { beforePage = blankBeforeImage(pageId); }
      beforePage.header().usedBytes = std::min<std::uint32_t>(
        beforePage.header().usedBytes, static_cast<std::uint32_t>(Page::kV2PayloadSize));
      beforePage.header().version = Page::kFormatVersionV2;

      const auto pageLsn = wal_.appendWithLsn(LogRecordType::PageImageWritten,
        [&](LogSequenceNumber assignedLsn) {
          afterPage.setPageLsn(assignedLsn.value());
          return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
            .transactionId = transaction.id(),
            .pageId = pageId,
            .previousTxLsn = previousTxLsn,
            .pageFormatVersion = Page::kFormatVersionV2,
            .beforeImage = beforePage.serialize(),
            .afterImage = afterPage.serialize(),
          });
        });
      previousTxLsn = pageLsn.value();
      afterPage.setPageLsn(pageLsn.value());
      files_.writePage(afterPage);
    }
    files_.flush();
    transactions_.commit(transaction.id());
  } catch (...) {
    files_.markFailed();
    throw;
  }
}

} // namespace zenthrildb
