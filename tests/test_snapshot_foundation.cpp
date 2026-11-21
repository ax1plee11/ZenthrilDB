#include "core/DatabaseRuntime.hpp"
#include "core/CRCManager.hpp"
#include "storage/BinaryFormat.hpp"
#include "storage/BinaryIO.hpp"
#include "storage/SnapshotManifest.hpp"
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

const std::vector<Byte> kOriginal{0, 42, 255};
const std::vector<Byte> kCommitted{0, 42, 254};

std::string readSnapshotFixture(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Failed to read snapshot foundation fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void damageSnapshotFixture(const std::filesystem::path& path, std::uintmax_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte{};
  if (!file.get(byte)) throw std::runtime_error("Failed to read snapshot foundation fixture byte");
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(static_cast<unsigned char>(byte) ^ 0x80));
  file.close();
  if (!file) throw std::runtime_error("Failed to damage snapshot foundation fixture");
}

void copySnapshot(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::filesystem::copy_file(from, to);
  std::filesystem::copy_file(databaseWalPath(from), databaseWalPath(to));
  std::filesystem::copy_file(snapshotBaselinePath(from), snapshotBaselinePath(to));
  std::filesystem::copy_file(snapshotManifestPath(from), snapshotManifestPath(to));
}

void removeSnapshotFixture(const std::filesystem::path& path) {
  removeTestDatabaseArtifacts(path);
  std::filesystem::remove(snapshotBaselinePath(path));
  std::filesystem::remove(snapshotManifestPath(path));
}

std::vector<std::string> snapshotBytes(const std::filesystem::path& path) {
  return {
    readSnapshotFixture(path),
    readSnapshotFixture(databaseWalPath(path)),
    readSnapshotFixture(snapshotBaselinePath(path)),
    readSnapshotFixture(snapshotManifestPath(path)),
  };
}

template <typename Operation>
void assertSnapshotRejected(const std::filesystem::path& path, Operation&& operation) {
  const auto before = snapshotBytes(path);
  bool rejected = false;
  try { operation(); }
  catch (const std::exception&) { rejected = true; }
  assert(rejected);
  assert(snapshotBytes(path) == before);
}

void stagePageMutation(const std::filesystem::path& path, PageId id, Byte value, bool commit) {
  FileManager files;
  files.openDatabase(path);
  LogManager log(databaseWalPath(path), files.header(), files.walIdentity(), false, files.ownership());
  TransactionManager transactions(log);
  const auto tx = transactions.begin();
  const auto before = files.readPage(id);
  auto after = before;
  assert(after.header().pageType == PageType::Overflow && after.header().usedBytes > 0);
  after.payload()[after.header().usedBytes - 1] = value;
  (void)log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber assigned) {
    after.setPageLsn(assigned.value());
    return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
      .transactionId = tx.id(),
      .pageId = id,
      .previousTxLsn = 0,
      .pageFormatVersion = Page::kFormatVersionV2,
      .beforeImage = before.serialize(),
      .afterImage = after.serialize(),
    });
  });
  if (commit) {
    transactions.commit(tx.id());
  } else {
    files.writePage(after);
    files.flush();
  }
}

void assertRows(const std::filesystem::path& path, const std::vector<Byte>& changed) {
  DatabaseRuntime database(path);
  assert(database.records().get("records", "untouched") == kOriginal);
  assert(database.records().get("records", "changed") == changed);
  assert(!database.records().get("records", "erased"));
  assert(database.records().scan("records").size() == 2);
  (void)database.checkConsistency();
}

void checkSidecarsOnly(const std::filesystem::path& snapshot, const std::filesystem::path& copied,
                       const WalIdentity& identity) {
  const auto baseline = snapshotBaselinePath(copied);
  const auto manifest = snapshotManifestPath(copied);
  std::filesystem::copy_file(snapshotBaselinePath(snapshot), baseline);
  std::filesystem::copy_file(snapshotManifestPath(snapshot), manifest);
  const auto baselineBefore = readSnapshotFixture(baseline);
  const auto manifestBefore = readSnapshotFixture(manifest);
  for (int open = 0; open < 2; ++open) {
    assert(DatabaseRuntime::verifySnapshot(copied, true).identity == identity);
  }
  assert(!std::filesystem::exists(copied));
  assert(!std::filesystem::exists(databaseWalPath(copied)));
  auto lockPath = copied;
  lockPath += ".lock";
  assert(!std::filesystem::exists(lockPath));
  assert(readSnapshotFixture(baseline) == baselineBefore);
  assert(readSnapshotFixture(manifest) == manifestBefore);
  std::filesystem::remove(baseline);
  std::filesystem::remove(manifest);
}

void checkRepair(const std::filesystem::path& copy, PageId changed, PageId untouched) {
  const auto before = snapshotBytes(copy);
  stagePageMutation(copy, changed, kCommitted.back(), true);
  assert(readSnapshotFixture(copy) == before[0]);
  const auto afterCommitWal = readSnapshotFixture(databaseWalPath(copy));
  assert(afterCommitWal.starts_with(before[1]) && afterCommitWal.size() > before[1].size());
  assertRows(copy, kCommitted);
  const auto committedData = readSnapshotFixture(copy);
  assert(committedData != before[0]);

  stagePageMutation(copy, changed, 253, false);
  assert(readSnapshotFixture(copy) != committedData);
  assertRows(copy, kCommitted);
  assert(readSnapshotFixture(copy) == committedData);

  const auto walBeforeRepair = readSnapshotFixture(databaseWalPath(copy));
  const auto dataOffset = DatabaseHeader::kSerializedSize +
    static_cast<std::uintmax_t>(untouched) * kDefaultPageSize + kDefaultPageSize - 17;
  damageSnapshotFixture(copy, dataOffset);
  assert(readSnapshotFixture(copy) != committedData);
  assertSnapshotRejected(copy, [&] { (void)DatabaseRuntime::verifySnapshot(copy); });
  for (int reopen = 0; reopen < 2; ++reopen) {
    assertRows(copy, kCommitted);
    assert(readSnapshotFixture(copy) == committedData);
    assert(readSnapshotFixture(databaseWalPath(copy)) == walBeforeRepair);
    assert(readSnapshotFixture(snapshotBaselinePath(copy)) == before[2]);
    assert(readSnapshotFixture(snapshotManifestPath(copy)) == before[3]);
  }
}

void checkInvalidBaselineStructure(const std::filesystem::path& snapshot, const std::filesystem::path& copy,
                                   PageId pageId, std::size_t fieldOffset, PageId replacement) {
  copySnapshot(snapshot, copy);
  const auto baseline = snapshotBaselinePath(copy);
  const auto manifestPath = snapshotManifestPath(copy);
  auto manifest = readSnapshotManifest(manifestPath);

  // Preserve the page and manifest checksums so verification must also inspect
  // catalog and free-list relationships.
  const auto offset = DatabaseHeader::kSerializedSize + static_cast<std::uintmax_t>(pageId) * kDefaultPageSize;
  std::fstream file(baseline, std::ios::binary | std::ios::in | std::ios::out);
  std::array<Byte, kDefaultPageSize> bytes{};
  file.seekg(static_cast<std::streamoff>(offset));
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  assert(file);
  Page page;
  page.deserialize(bytes);
  binary::writeLittleEndian(page.payload(), fieldOffset, replacement);
  bytes = page.serialize();
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.close();
  assert(file);

  manifest.databaseCrc = snapshotFileCrc(baseline, manifest.databaseBytes);
  writeSnapshotManifest(manifestPath, manifest);
  assertSnapshotRejected(copy, [&] { (void)DatabaseRuntime::verifySnapshot(copy, true); });
  assertSnapshotRejected(copy, [&] { DatabaseRuntime database(copy); });
  removeSnapshotFixture(copy);
}

void checkInvalidPostBoundaryStructure(const std::filesystem::path& snapshot,
                                       const std::filesystem::path& copy,
                                       PageId pageId, std::size_t fieldOffset, PageId replacement) {
  copySnapshot(snapshot, copy);
  const auto baseline = DatabaseRuntime::verifySnapshot(copy, true);
  {
    FileManager files;
    files.openDatabase(copy);
    LogManager log(databaseWalPath(copy), files.header(), files.walIdentity(), false, files.ownership());
    TransactionManager transactions(log);
    const auto tx = transactions.begin();
    const auto before = files.readPage(pageId);
    auto after = before;
    binary::writeLittleEndian(after.payload(), fieldOffset, replacement);
    const auto assigned = log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber lsn) {
      after.setPageLsn(lsn.value());
      return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
        .transactionId = tx.id(),
        .pageId = pageId,
        .previousTxLsn = 0,
        .pageFormatVersion = Page::kFormatVersionV2,
        .beforeImage = before.serialize(),
        .afterImage = after.serialize(),
      });
    });
    assert(assigned.value() > baseline.identity.snapshotLsn);
    transactions.commit(tx.id());
    files.writePage(after);
    files.flush();
  }
  // The baseline is valid and the working page matches the committed WAL image,
  // including its CRC and LSN. Only post-replay structural validation rejects it.
  const auto beforeVerification = snapshotBytes(copy);
  assert(DatabaseRuntime::verifySnapshot(copy, true) == baseline);
  assert(snapshotBytes(copy) == beforeVerification);
  assertSnapshotRejected(copy, [&] { (void)DatabaseRuntime::verifySnapshot(copy); });
  removeSnapshotFixture(copy);
}

void checkPostBoundaryGrowth(const std::filesystem::path& snapshot, const std::filesystem::path& directory) {
  const auto working = directory / "growing.zdb";
  copySnapshot(snapshot, working);
  const auto base = DatabaseRuntime::verifySnapshot(working, true);
  const auto baseBytes = readSnapshotFixture(snapshotBaselinePath(working));
  const auto manifestBytes = readSnapshotFixture(snapshotManifestPath(working));
  std::uint64_t grownCount{};
  PageId reusablePage{};
  {
    DatabaseRuntime database(working);
    reusablePage = database.createTable("scratch").rootPageId();
    for (int i = 0; i < 4; ++i) database.records().put("scratch", std::to_string(i), kCommitted);
    grownCount = database.checkConsistency().pagesChecked;
    assert(grownCount > base.pageCount);
    assert(database.records().scan("scratch").size() == 4);
    assert(database.records().get("records", "untouched") == kOriginal);
  }
  {
    DatabaseRuntime database(working);
    assert(database.records().get("scratch", "3") == kCommitted);
    assert(database.dropTable("scratch"));
    assert(database.createTable("reused").rootPageId() == reusablePage);
    database.records().put("reused", "survivor", kCommitted);
    assert(database.renameTable("records", "renamed"));
    assert(database.checkConsistency().pagesChecked == grownCount);
    const auto checkpoint = database.checkpoint();
    assert(checkpoint.durableWalLsn.value() > base.identity.snapshotLsn);
  }
  const auto verifyRows = [&](const std::filesystem::path& path) {
    DatabaseRuntime database(path);
    assert(!database.tableExists("scratch") && !database.tableExists("records"));
    assert(database.records().get("reused", "survivor") == kCommitted);
    assert(database.records().get("renamed", "untouched") == kOriginal);
    assert(database.records().get("renamed", "changed") == kOriginal);
    assert(database.checkConsistency().pagesChecked == grownCount);
  };
  verifyRows(working);
  assert(DatabaseRuntime::verifySnapshot(working) == base);
  assert(readSnapshotFixture(snapshotBaselinePath(working)) == baseBytes);
  assert(readSnapshotFixture(snapshotManifestPath(working)) == manifestBytes);
  const auto workingBytes = snapshotBytes(working);

  const auto childDirectory = directory / "child";
  const auto child = childDirectory / working.filename();
  DatabaseRuntime::snapshotTo(working, childDirectory);
  const auto childManifest = DatabaseRuntime::verifySnapshot(child);
  assert(childManifest.pageCount == grownCount);
  assert(childManifest.identity.databaseUuid == base.identity.databaseUuid);
  assert(childManifest.identity.generation != base.identity.generation);
  assert(childManifest.identity.snapshotLsn > base.identity.snapshotLsn);
  assert(snapshotBytes(working) == workingBytes);
  verifyRows(child);

  const auto expectedData = readSnapshotFixture(child);
  const auto pairOnly = directory / "pair-only.zdb";
  std::filesystem::copy_file(child, pairOnly);
  std::filesystem::copy_file(databaseWalPath(child), databaseWalPath(pairOnly));
  const auto pairWal = readSnapshotFixture(databaseWalPath(pairOnly));
  // Force full history to restore images on both sides of the snapshot boundary.
  const auto offset = [](PageId page) {
    return DatabaseHeader::kSerializedSize + static_cast<std::uintmax_t>(page) * kDefaultPageSize
      + kDefaultPageSize - 17;
  };
  damageSnapshotFixture(pairOnly, offset(kDatabaseMetadataPageId));
  damageSnapshotFixture(pairOnly, offset(static_cast<PageId>(grownCount - 1)));
  assert(readSnapshotFixture(pairOnly) != expectedData);
  for (int open = 0; open < 2; ++open) {
    verifyRows(pairOnly);
    assert(readSnapshotFixture(pairOnly) == expectedData);
    assert(readSnapshotFixture(databaseWalPath(pairOnly)) == pairWal);
  }
  removeTestDatabaseArtifacts(pairOnly);
  removeSnapshotFixture(child);
  std::filesystem::remove(childDirectory);
  removeSnapshotFixture(working);
}

void checkIncompleteSidecars(const std::filesystem::path& snapshot, const std::filesystem::path& directory) {
  const auto originalData = readSnapshotFixture(snapshot);
  const auto originalWal = readSnapshotFixture(databaseWalPath(snapshot));
  for (const bool keepManifest : {false, true}) {
    const auto path = directory / (keepManifest ? "manifest-only.zdb" : "baseline-only.zdb");
    const auto remaining = keepManifest ? snapshotManifestPath(path) : snapshotBaselinePath(path);
    const auto missing = keepManifest ? snapshotBaselinePath(path) : snapshotManifestPath(path);
    const auto originalSidecar = keepManifest ? snapshotManifestPath(snapshot) : snapshotBaselinePath(snapshot);
    std::filesystem::copy_file(originalSidecar, remaining);
    const auto sidecar = readSnapshotFixture(remaining);
    bool creationRejected = false;
    try { DatabaseRuntime database(path, true); }
    catch (const std::exception&) { creationRejected = true; }
    assert(creationRejected);
    assert(!std::filesystem::exists(path) && !std::filesystem::exists(databaseWalPath(path)));
    assert(!std::filesystem::exists(missing));
    assert(readSnapshotFixture(remaining) == sidecar);

    std::filesystem::copy_file(snapshot, path);
    std::filesystem::copy_file(databaseWalPath(snapshot), databaseWalPath(path));
    bool openingRejected = false;
    try { DatabaseRuntime database(path); }
    catch (const std::exception&) { openingRejected = true; }
    assert(openingRejected);
    assert(readSnapshotFixture(path) == originalData);
    assert(readSnapshotFixture(databaseWalPath(path)) == originalWal);
    assert(!std::filesystem::exists(missing));
    assert(readSnapshotFixture(remaining) == sidecar);
    removeSnapshotFixture(path);
  }
}

void checkBaselineWithoutHistoricalImages(const std::filesystem::path& snapshot,
                                          const std::filesystem::path& directory) {
  const auto working = directory / "synthetic-prefix.zdb";
  copySnapshot(snapshot, working);
  const auto manifestPath = snapshotManifestPath(working);
  const auto baselinePath = snapshotBaselinePath(working);
  const auto walPath = databaseWalPath(working);
  auto manifest = readSnapshotManifest(manifestPath);
  const auto boundary = manifest.identity.snapshotLsn;
  assert(boundary > 2 && manifest.transactionHighWater > 0);

  // This intentionally builds an independent test log with no prior page or
  // header images. The original published snapshot is unchanged.
  const auto tx = LogPayloadCodec::encodeTransaction(TransactionPayload{
    .transactionId = TransactionId{manifest.transactionHighWater},
  });
  const std::array frames{
    LogRecord{LogSequenceNumber{boundary - 2}, LogRecordType::TransactionStarted, tx}.serialize(),
    LogRecord{LogSequenceNumber{boundary - 1}, LogRecordType::TransactionCommitted, tx}.serialize(),
    LogRecord{LogSequenceNumber{boundary}, LogRecordType::Checkpoint, {}}.serialize(),
  };
  {
    std::ofstream wal(walPath, std::ios::binary | std::ios::trunc);
    const auto identity = manifest.identity.serialize();
    wal.write(reinterpret_cast<const char*>(identity.data()), static_cast<std::streamsize>(identity.size()));
    for (const auto& frame : frames) {
      wal.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    }
    wal.close();
    assert(wal);
  }
  manifest.walPrefixBytes = std::filesystem::file_size(walPath);
  manifest.walPrefixCrc = snapshotFileCrc(walPath, manifest.walPrefixBytes);
  writeSnapshotManifest(manifestPath, manifest);
  const auto syntheticWal = readSnapshotFixture(walPath);
  const auto syntheticManifest = readSnapshotFixture(manifestPath);
  const auto baseline = readSnapshotFixture(baselinePath);
  constexpr auto prefixAndIdentityPage = DatabaseHeader::kSerializedSize + kDefaultPageSize;

  std::filesystem::resize_file(working, prefixAndIdentityPage);
  for (int open = 0; open < 2; ++open) {
    assertRows(working, kOriginal);
    assert(readSnapshotFixture(working) == baseline);
    assert(readSnapshotFixture(walPath) == syntheticWal);
  }
  {
    DatabaseRuntime database(working);
    database.records().put("records", "new", kCommitted);
    const auto checkpoint = database.checkpoint();
    assert(checkpoint.durableWalLsn.value() > boundary);
    assert(checkpoint.transactionHighWater > manifest.transactionHighWater);
  }

  const auto expectedData = readSnapshotFixture(working);
  const auto completeWal = readSnapshotFixture(walPath);
  assert(completeWal.starts_with(syntheticWal) && completeWal.size() > syntheticWal.size());
  const auto nextFrame = LogRecord{LogSequenceNumber{1}, LogRecordType::Checkpoint, {}}.serialize();
  {
    std::ofstream wal(walPath, std::ios::binary | std::ios::app);
    wal.write(reinterpret_cast<const char*>(nextFrame.data()), 3);
    wal.close();
    assert(wal);
  }
  std::filesystem::resize_file(working, prefixAndIdentityPage);
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime database(working);
    assert(database.records().get("records", "untouched") == kOriginal);
    assert(database.records().get("records", "changed") == kOriginal);
    assert(database.records().get("records", "new") == kCommitted);
    assert(database.records().scan("records").size() == 3);
    (void)database.checkConsistency();
    assert(readSnapshotFixture(working) == expectedData);
    assert(readSnapshotFixture(walPath) == completeWal);
  }
  assert(readSnapshotFixture(manifestPath) == syntheticManifest);
  assert(readSnapshotFixture(baselinePath) == baseline);
  removeSnapshotFixture(working);
}

void checkRequiredBaseline(const std::filesystem::path& source, const std::filesystem::path& directory) {
  const auto published = directory / "required";
  const auto database = published / source.filename();
  const auto originalData = readSnapshotFixture(source);
  const auto originalWal = readSnapshotFixture(databaseWalPath(source));
  DatabaseRuntime::snapshotTo(source, published, true);
  const auto manifest = DatabaseRuntime::verifySnapshot(database);
  assert(manifest.identity.baselineRequired);
  assert(WalIdentity::deserialize(manifest.identity.serialize()) == manifest.identity);
  for (const auto flag : {0u, 2u}) {
    auto encoded = manifest.identity.serialize();
    std::size_t flagOffset = 28;
    binary::writeLittleEndian(encoded, flagOffset, flag);
    std::size_t checksumOffset = WalIdentity::kSize - sizeof(std::uint32_t);
    const auto checksum = CRCManager::compute(std::span<const Byte>(encoded).first(checksumOffset));
    binary::writeLittleEndian(encoded, checksumOffset, checksum);
    bool rejected = false;
    try { (void)WalIdentity::deserialize(encoded); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
  }
  assert(readSnapshotFixture(source) == originalData);
  assert(readSnapshotFixture(databaseWalPath(source)) == originalWal);

  const auto backup = directory / "required-backup";
  {
    DatabaseRuntime runtime(database);
    runtime.records().put("records", "post-boundary", kCommitted);
    (void)runtime.checkpoint();
    runtime.backupTo(backup);
  }
  const auto backedUp = backup / source.filename();
  assert(DatabaseRuntime::verifySnapshot(backedUp).identity.baselineRequired);
  assert(snapshotBytes(backedUp) == snapshotBytes(database));
  {
    DatabaseRuntime runtime(backedUp);
    assert(runtime.records().get("records", "post-boundary") == kCommitted);
  }

  for (int missing = 1; missing <= 3; ++missing) {
    const auto copy = directory / ("missing-required-" + std::to_string(missing) + ".zdb");
    copySnapshot(database, copy);
    if (missing & 1) std::filesystem::remove(snapshotBaselinePath(copy));
    if (missing & 2) std::filesystem::remove(snapshotManifestPath(copy));
    if (missing == 3) {
      std::ofstream wal(databaseWalPath(copy), std::ios::binary | std::ios::app);
      wal.write("bad", 3);
      wal.close();
      assert(wal);
    }
    const auto data = readSnapshotFixture(copy);
    const auto wal = readSnapshotFixture(databaseWalPath(copy));
    for (int reopen = 0; reopen < 2; ++reopen) {
      bool rejected = false;
      try { DatabaseRuntime runtime(copy); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
      assert(readSnapshotFixture(copy) == data);
      assert(readSnapshotFixture(databaseWalPath(copy)) == wal);
    }
    removeSnapshotFixture(copy);
  }

  const auto childDirectory = directory / "required-child";
  DatabaseRuntime::snapshotTo(database, childDirectory);
  const auto child = childDirectory / source.filename();
  const auto childManifest = DatabaseRuntime::verifySnapshot(child);
  assert(childManifest.identity.baselineRequired);
  assert(childManifest.identity.databaseUuid == manifest.identity.databaseUuid);
  assert(childManifest.identity.generation != manifest.identity.generation);
  {
    DatabaseRuntime runtime(child);
    assert(runtime.records().get("records", "post-boundary") == kCommitted);
  }
  for (const auto& file : {child, backedUp, database}) removeSnapshotFixture(file);
  for (const auto& folder : {childDirectory, backup, published}) std::filesystem::remove(folder);
}

} // namespace

void test_snapshot_foundation() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_foundation_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto source = directory / "source.zdb";
  const auto output = directory / "published";
  const auto snapshot = output / source.filename();
  const auto repaired = directory / "repair.zdb";
  PageId root{};
  {
    DatabaseRuntime database(source, true);
    root = database.createTable("records").rootPageId();
    database.records().put("records", "untouched", kOriginal);
    database.records().put("records", "changed", kOriginal);
    database.records().put("records", "erased", kOriginal);
    assert(database.records().erase("records", "erased"));
  }
  PageId changed{};
  PageId untouched{};
  {
    FileManager files;
    files.openDatabase(source);
    const auto rootPage = files.readPage(root);
    std::size_t offset = sizeof(std::uint32_t);
    changed = binary::readLittleEndian<PageId>(rootPage.payload(), offset);
    const auto changedPage = files.readPage(changed);
    offset = sizeof(std::uint32_t);
    untouched = binary::readLittleEndian<PageId>(changedPage.payload(), offset);
    assert(changed >= kFirstUserPageId && untouched >= kFirstUserPageId && changed != untouched);
  }
  const auto sourceData = readSnapshotFixture(source);
  const auto sourceWal = readSnapshotFixture(databaseWalPath(source));
  DatabaseRuntime::snapshotTo(source, output);
  assert(readSnapshotFixture(source) == sourceData);
  assert(readSnapshotFixture(databaseWalPath(source)) == sourceWal);
  const auto snapshotBefore = snapshotBytes(snapshot);
  const auto identity = DatabaseRuntime::verifySnapshot(snapshot).identity;
  assert(DatabaseRuntime::verifySnapshot(snapshot, true).identity == identity);
  assert(snapshotBytes(snapshot) == snapshotBefore);
  checkSidecarsOnly(snapshot, directory / "sidecars-only.zdb", identity);

  copySnapshot(snapshot, repaired);
  checkRepair(repaired, changed, untouched);
  removeSnapshotFixture(repaired);

  const auto badManifest = directory / "bad-manifest.zdb";
  const auto badBaseline = directory / "bad-baseline.zdb";
  const auto badWalPrefix = directory / "bad-wal-prefix.zdb";
  copySnapshot(snapshot, badManifest);
  damageSnapshotFixture(snapshotManifestPath(badManifest), 0);
  assertSnapshotRejected(badManifest, [&] { (void)DatabaseRuntime::verifySnapshot(badManifest, true); });
  assertSnapshotRejected(badManifest, [&] { DatabaseRuntime database(badManifest); });
  removeSnapshotFixture(badManifest);
  copySnapshot(snapshot, badBaseline);
  const auto baselineOffset = DatabaseHeader::kSerializedSize +
    static_cast<std::uintmax_t>(kDatabaseMetadataPageId) * kDefaultPageSize + kDefaultPageSize - 17;
  damageSnapshotFixture(snapshotBaselinePath(badBaseline), baselineOffset);
  assertSnapshotRejected(badBaseline, [&] { (void)DatabaseRuntime::verifySnapshot(badBaseline, true); });
  assertSnapshotRejected(badBaseline, [&] { DatabaseRuntime database(badBaseline); });
  removeSnapshotFixture(badBaseline);
  copySnapshot(snapshot, badWalPrefix);
  const auto walPrefixLength = readSnapshotManifest(snapshotManifestPath(badWalPrefix)).walPrefixBytes;
  assert(walPrefixLength > WalIdentity::kSize);
  damageSnapshotFixture(databaseWalPath(badWalPrefix), walPrefixLength - 1);
  const auto brokenWalPair = snapshotBytes(badWalPrefix);
  assert(DatabaseRuntime::verifySnapshot(badWalPrefix, true).identity == identity);
  assert(snapshotBytes(badWalPrefix) == brokenWalPair);
  assertSnapshotRejected(badWalPrefix, [&] { (void)DatabaseRuntime::verifySnapshot(badWalPrefix); });
  assertSnapshotRejected(badWalPrefix, [&] { DatabaseRuntime database(badWalPrefix); });
  removeSnapshotFixture(badWalPrefix);

  checkInvalidBaselineStructure(snapshot, directory / "bad-catalog.zdb", kPageDirectoryPageId,
                                 format::kPageDirectoryEntriesOffset, PageId{0});
  checkInvalidBaselineStructure(snapshot, directory / "bad-free-list.zdb", kFreePageListPageId,
                                 format::kFreePageListEntriesOffset, untouched);
  checkInvalidPostBoundaryStructure(snapshot, directory / "bad-live-catalog.zdb", kPageDirectoryPageId,
                                    format::kPageDirectoryEntriesOffset, PageId{0});
  checkInvalidPostBoundaryStructure(snapshot, directory / "bad-live-free-list.zdb", kFreePageListPageId,
                                    format::kFreePageListEntriesOffset, untouched);
  checkPostBoundaryGrowth(snapshot, directory);
  checkIncompleteSidecars(snapshot, directory);
  checkBaselineWithoutHistoricalImages(snapshot, directory);
  checkRequiredBaseline(source, directory);

  assert(snapshotBytes(snapshot) == snapshotBefore);
  assert(readSnapshotFixture(source) == sourceData);
  assert(readSnapshotFixture(databaseWalPath(source)) == sourceWal);
  removeSnapshotFixture(snapshot);
  std::filesystem::remove(output);
  removeTestDatabaseArtifacts(source);
  std::filesystem::remove(directory);
}
