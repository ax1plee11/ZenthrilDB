#include "core/CRCManager.hpp"
#include "core/DatabaseRuntime.hpp"
#include "storage/BinaryIO.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogPayload.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zenthrildb;

namespace {

const std::vector<Byte> kValue{7, 0, 255};

struct FixturePages {
  PageId livePage;
  PageId freePage;
  std::uint64_t pageCount;
};

std::uintmax_t pageOffset(PageId pageId) {
  return DatabaseHeader::kSerializedSize + static_cast<std::uintmax_t>(pageId) * kDefaultPageSize;
}

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Failed to read physical recovery fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeAt(const std::filesystem::path& path, std::uintmax_t offset, std::span<const Byte> bytes) {
  std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  stream.close();
  if (!stream) throw std::runtime_error("Failed to write physical recovery fixture");
}

void damagePageChecksum(const std::filesystem::path& path, PageId pageId) {
  const auto offset = pageOffset(pageId) + kDefaultPageSize - 17;
  std::ifstream stream(path, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(offset));
  char byte{};
  if (!stream.get(byte)) throw std::runtime_error("Failed to read physical recovery fixture byte");
  stream.close();
  const Byte damaged = static_cast<Byte>(static_cast<Byte>(byte) ^ Byte{0x80});
  writeAt(path, offset, std::span<const Byte>(&damaged, 1));
}

void copyPair(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::filesystem::copy_file(source, destination);
  std::filesystem::copy_file(databaseWalPath(source), databaseWalPath(destination));
}

void assertRejectedUnchanged(const std::filesystem::path& path) {
  const auto walPath = databaseWalPath(path);
  const auto beforeData = readBytes(path);
  const auto beforeWal = readBytes(walPath);
  bool rejected = false;
  try {
    FileManager files;
    files.openDatabase(path);
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  assert(readBytes(path) == beforeData);
  assert(readBytes(walPath) == beforeWal);
}

FixturePages makeFixture(const std::filesystem::path& path) {
  PageId rootPageId{};
  {
    DatabaseRuntime database(path, true);
    rootPageId = database.createTable("records").rootPageId();
    database.records().put("records", "temporary", kValue);
    database.records().put("records", "durable", kValue);
    assert(database.records().erase("records", "temporary"));
    (void)database.checkConsistency();
  }
  FileManager files;
  files.openDatabase(path);
  const auto root = files.readPage(rootPageId);
  std::size_t nextOffset = sizeof(std::uint32_t);
  const auto liveId = binary::readLittleEndian<PageId>(root.payload(), nextOffset);
  const auto pageCount = files.physicalPageCount();
  assert(liveId >= kFirstUserPageId && static_cast<std::uint64_t>(liveId) + 1 == pageCount);
  for (PageId pageId = kFirstUserPageId; pageId < pageCount; ++pageId) {
    if (files.readPage(pageId).header().pageType == PageType::Free) return {liveId, pageId, pageCount};
  }
  throw std::runtime_error("Physical recovery fixture has no free page");
}

void assertValidTwice(const std::filesystem::path& path, const FixturePages& pages) {
  const auto wal = readBytes(databaseWalPath(path));
  for (int attempt = 0; attempt < 2; ++attempt) {
    DatabaseRuntime database(path);
    assert(database.records().get("records", "durable") == kValue);
    assert(!database.records().get("records", "temporary"));
    assert(database.checkConsistency().pagesChecked == pages.pageCount);
  }
  assert(readBytes(databaseWalPath(path)) == wal);
  FileManager files;
  files.openDatabase(path);
  assert(files.readPage(pages.freePage).header().pageType == PageType::Free);
  assert(files.readPage(pages.livePage).header().pageType == PageType::Overflow);
}

void stageUncommittedPageWrite(const std::filesystem::path& path, PageId pageId) {
  FileManager files;
  files.openDatabase(path);
  LogManager log(databaseWalPath(path), files.header(), files.walIdentity(), false, files.ownership());
  TransactionManager transactions(log);
  const auto tx = transactions.begin();
  const auto before = files.readPage(pageId);
  auto after = before;
  assert(after.header().usedBytes > 0);
  after.payload()[after.header().usedBytes - 1] ^= 0x20;
  const auto lsn = log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber assigned) {
    after.setPageLsn(assigned.value());
    return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
      .transactionId = tx.id(),
      .pageId = pageId,
      .previousTxLsn = 0,
      .pageFormatVersion = Page::kFormatVersionV2,
      .beforeImage = before.serialize(),
      .afterImage = after.serialize(),
    });
  });
  after.setPageLsn(lsn.value());
  files.writePage(after);
  files.flush();
}

void writeUnsupportedPageVersion(const std::filesystem::path& path, PageId pageId) {
  Page page;
  {
    FileManager files;
    files.openDatabase(path);
    page = files.readPage(pageId);
  }
  auto bytes = page.serialize();
  constexpr auto checksumOffset = 2 * sizeof(std::uint32_t);
  auto offset = PageHeader::kSerializedSize - sizeof(std::uint32_t);
  binary::writeLittleEndian(std::span<Byte>(bytes), offset, Page::kFormatVersionV2 + 1);
  offset = checksumOffset;
  binary::writeLittleEndian(std::span<Byte>(bytes), offset, std::uint32_t{0});
  offset = checksumOffset;
  binary::writeLittleEndian(std::span<Byte>(bytes), offset, CRCManager::compute(bytes));
  writeAt(path, pageOffset(pageId), bytes);
}

} // namespace

void test_physical_page_recovery() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_page_recovery_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto source = directory / "source.zdb";
  const auto committed = directory / "committed.zdb";
  const auto incomplete = directory / "incomplete.zdb";
  const auto partial = directory / "partial.zdb";
  const auto unsupported = directory / "unsupported.zdb";
  const auto identity = directory / "identity.zdb";
  const auto unmapped = directory / "unmapped.zdb";
  const auto foreign = directory / "foreign.zdb";
  const auto damagedWal = directory / "damaged-wal.zdb";
  const auto pages = makeFixture(source);
  const auto stableData = readBytes(source);

  copyPair(source, committed);
  for (const auto pageId : {pages.livePage, pages.freePage, kDatabaseMetadataPageId,
                            kPageDirectoryPageId, kFreePageListPageId}) {
    damagePageChecksum(committed, pageId);
  }
  assertValidTwice(committed, pages);
  assert(readBytes(committed) == stableData);

  copyPair(source, incomplete);
  stageUncommittedPageWrite(incomplete, pages.livePage);
  damagePageChecksum(incomplete, pages.livePage);
  assertValidTwice(incomplete, pages);
  assert(readBytes(incomplete) == stableData);

  copyPair(source, partial);
  std::filesystem::resize_file(partial, pageOffset(pages.livePage) + kDefaultPageSize / 2);
  assertValidTwice(partial, pages);
  assert(readBytes(partial) == stableData);

  copyPair(source, unsupported);
  writeUnsupportedPageVersion(unsupported, pages.livePage);
  assertRejectedUnchanged(unsupported);

  copyPair(source, identity);
  damagePageChecksum(identity, kHeaderMirrorPageId);
  assertRejectedUnchanged(identity);

  {
    FileManager files;
    files.createDatabase(unmapped, true);
  }
  damagePageChecksum(unmapped, kDatabaseMetadataPageId);
  {
    std::ofstream tail(databaseWalPath(unmapped), std::ios::binary | std::ios::app);
    tail.write("x", 1);
    tail.close();
    assert(tail);
  }
  assertRejectedUnchanged(unmapped);

  (void)makeFixture(foreign);
  damagePageChecksum(foreign, pages.livePage);
  std::filesystem::copy_file(databaseWalPath(source), databaseWalPath(foreign),
                             std::filesystem::copy_options::overwrite_existing);
  assertRejectedUnchanged(foreign);

  copyPair(source, damagedWal);
  damagePageChecksum(damagedWal, pages.livePage);
  {
    const auto path = databaseWalPath(damagedWal);
    const auto wal = readBytes(path);
    const auto offset = WalIdentity::kSize + LogRecord::kHeaderSize * 2 + 24;
    const auto changed = static_cast<Byte>(static_cast<Byte>(wal.at(offset)) ^ Byte{0x80});
    writeAt(path, offset, std::span<const Byte>(&changed, 1));
  }
  assertRejectedUnchanged(damagedWal);

  for (const auto& path : {source, committed, incomplete, partial, unsupported, identity, unmapped, foreign, damagedWal}) {
    removeTestDatabaseArtifacts(path);
  }
  std::filesystem::remove(directory);
}
