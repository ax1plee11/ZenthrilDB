#include "core/CRCManager.hpp"
#include "core/FileManager.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

using namespace zenthrildb;

namespace {
std::array<Byte, DatabaseHeader::serializedSize()> checksumHeader(DatabaseHeader header) {
  header.checksum = 0;
  const auto checksumInput = header.serialize();
  header.checksum = CRCManager::compute(checksumInput);
  return header.serialize();
}

void writeGrowth(const std::filesystem::path& path, Byte value, bool commit, PageId pages) {
  FileManager files;
  files.openDatabase(path);
  LogManager wal(databaseWalPath(path), files.ownership());
  TransactionManager transactions(wal);
  const auto transaction = transactions.begin();

  const auto beforeHeader = files.header();
  auto afterHeader = beforeHeader;
  afterHeader.totalPages += pages;
  const auto beforeHeaderImage = beforeHeader.serialize();
  const auto afterHeaderImage = checksumHeader(afterHeader);
  auto previousLsn = wal.appendWithLsn(LogRecordType::DatabaseHeaderImageWritten,
    [&](LogSequenceNumber) {
      return LogPayloadCodec::encodeDatabaseHeaderImageWritten(DatabaseHeaderImageWrittenPayload{
        .transactionId = transaction.id(),
        .previousTxLsn = 0,
        .beforeImage = beforeHeaderImage,
        .afterImage = afterHeaderImage,
      });
    });
  files.restoreHeaderImage(afterHeaderImage, false);

  for (PageId offset = 0; offset < pages; ++offset) {
    const auto pageId = kFirstUserPageId + offset;
    Page beforePage;
    beforePage.header().version = Page::kFormatVersionV2;
    beforePage.header().pageId = pageId;
    beforePage.header().pageType = PageType::Free;
    Page afterPage = beforePage;
    afterPage.header().pageType = PageType::Table;
    afterPage.header().usedBytes = 1;
    afterPage.payload()[0] = value;
    const auto pageLsn = wal.appendWithLsn(LogRecordType::PageImageWritten,
      [&](LogSequenceNumber assigned) {
        afterPage.setPageLsn(assigned.value());
        return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
          .transactionId = transaction.id(),
          .pageId = pageId,
          .previousTxLsn = previousLsn.value(),
          .pageFormatVersion = Page::kFormatVersionV2,
          .beforeImage = beforePage.serialize(),
          .afterImage = afterPage.serialize(),
        });
      });
    afterPage.setPageLsn(pageLsn.value());
    files.writePage(afterPage);
    previousLsn = pageLsn;
  }
  files.flush();
  if (commit) transactions.commit(transaction.id());
}

void runHeaderRecoveryCase(bool commit) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
    ("zenthrildb_header_recovery_" + std::to_string(nonce) + ".zdb");
  removeTestDatabaseArtifacts(path);
  {
    FileManager files;
    files.createDatabase(path);
    files.closeDatabase();
  }
  writeGrowth(path, 0x5A, commit, commit ? 1 : 2);
  {
    FileManager reopened;
    reopened.openDatabase(path);
    if (commit) {
      assert(reopened.header().totalPages == kFirstUserPageId + 1);
      assert(reopened.readPage(kFirstUserPageId).payload()[0] == 0x5A);
    } else {
      assert(reopened.header().totalPages == kFirstUserPageId);
      assert(reopened.physicalPageCount() == kFirstUserPageId);
    }
  }
  if (!commit) {
    // Retry the growth at the same page id after the first recovery rolled it back.
    writeGrowth(path, 0x6B, true, 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      FileManager reopened;
      reopened.openDatabase(path);
      assert(reopened.header().totalPages == kFirstUserPageId + 1);
      assert(reopened.physicalPageCount() == kFirstUserPageId + 1);
      assert(reopened.readPage(kFirstUserPageId).payload()[0] == 0x6B);
    }
  }
  removeTestDatabaseArtifacts(path);
}
} // namespace

void test_header_recovery() {
  runHeaderRecoveryCase(true);
  runHeaderRecoveryCase(false);
}
