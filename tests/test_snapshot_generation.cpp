#include "core/CRCManager.hpp"
#include "core/DatabaseRuntime.hpp"
#include "storage/BinaryIO.hpp"
#include "storage/WalIdentity.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogPayload.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace zenthrildb;

namespace {

const std::vector<Byte> kValue{0, 2, 255};

std::vector<Byte> readFixture(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read snapshot test fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void writeFixture(const std::filesystem::path& path, std::span<const Byte> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file) throw std::runtime_error("Failed to write snapshot test fixture");
}

void copyPair(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::filesystem::copy_file(source, destination);
  std::filesystem::copy_file(databaseWalPath(source), databaseWalPath(destination));
}

void assertOpenPreservesFilesOnRejection(const std::filesystem::path& database, std::string_view expected = {}) {
  const auto wal = databaseWalPath(database);
  const auto dataBefore = readFixture(database);
  const auto walBefore = readFixture(wal);
  bool rejected = false;
  try {
    FileManager files;
    files.openDatabase(database);
  } catch (const std::exception& error) {
    rejected = std::string_view(error.what()).find(expected) != std::string_view::npos;
  }
  assert(rejected);
  assert(readFixture(database) == dataBefore);
  assert(readFixture(wal) == walBefore);
}

void assertStandaloneCannotAppend(const std::filesystem::path& wal) {
  const auto before = readFixture(wal);
  bool rejected = false;
  try {
    LogManager standalone(wal);
    (void)standalone.checkpoint();
  } catch (const std::exception&) {
    rejected = true;
  }
  assert(rejected);
  assert(readFixture(wal) == before);
}

WalIdentity identityOf(const std::filesystem::path& database) {
  FileManager files;
  files.openDatabase(database);
  assert(files.walIdentity().has_value());
  return *files.walIdentity();
}

void rewriteIdentityChecksum(std::span<Byte> identityBytes) {
  const auto checksumOffset = WalIdentity::kSize - sizeof(std::uint32_t);
  auto offset = checksumOffset;
  binary::writeLittleEndian(identityBytes, offset, CRCManager::compute(identityBytes.first(checksumOffset)));
}

void checkTornTails(const std::filesystem::path& source, const std::filesystem::path& other,
                    const std::filesystem::path& directory) {
  const auto originalWal = readFixture(databaseWalPath(source));
  const auto repaired = directory / "repair.zdb";
  const auto mismatch = directory / "mismatch.zdb";
  const auto partial = LogRecord{LogSequenceNumber{1}, LogRecordType::Checkpoint, {}}.serialize();
  auto incomplete = originalWal;
  incomplete.insert(incomplete.end(), partial.begin(), partial.begin() + 3);
  copyPair(source, repaired);
  copyPair(other, mismatch);
  writeFixture(databaseWalPath(repaired), incomplete);
  writeFixture(databaseWalPath(mismatch), incomplete);
  assertStandaloneCannotAppend(databaseWalPath(repaired));
  assertOpenPreservesFilesOnRejection(mismatch);
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime runtime(repaired);
    assert(runtime.records().get("records", "key") == kValue);
    assert(readFixture(databaseWalPath(repaired)) == originalWal);
  }
  removeTestDatabaseArtifacts(repaired);
  removeTestDatabaseArtifacts(mismatch);
}

void checkMalformedIdentities(const std::filesystem::path& source, const std::filesystem::path& directory) {
  const auto originalWal = readFixture(databaseWalPath(source));
  const auto fixture = directory / "bad-identity.zdb";
  const auto fixtureWal = databaseWalPath(fixture);
  copyPair(source, fixture);
  for (const auto size : {std::size_t{3}, WalIdentity::kSize - 1}) {
    writeFixture(fixtureWal, std::span<const Byte>(originalWal).first(size));
    assertOpenPreservesFilesOnRejection(fixture);
  }
  assertStandaloneCannotAppend(fixtureWal);

  auto invalidWal = originalWal;
  invalidWal[WalIdentity::kSize - 1] ^= 0x01;
  writeFixture(fixtureWal, invalidWal);
  assertOpenPreservesFilesOnRejection(fixture);

  invalidWal = originalWal;
  std::size_t versionOffset = sizeof(std::uint32_t);
  binary::writeLittleEndian(std::span<Byte>(invalidWal), versionOffset, WalIdentity::kVersion + 1);
  rewriteIdentityChecksum(invalidWal);
  writeFixture(fixtureWal, invalidWal);
  assertOpenPreservesFilesOnRejection(fixture);

  invalidWal = originalWal;
  constexpr auto generationOffset = 3 * sizeof(std::uint32_t);
  std::fill_n(invalidWal.begin() + generationOffset, WalIdentity{}.generation.size(), Byte{0});
  rewriteIdentityChecksum(invalidWal);
  writeFixture(fixtureWal, invalidWal);
  assertOpenPreservesFilesOnRejection(fixture);

  writeFixture(fixtureWal, originalWal);
  auto data = readFixture(source);
  auto pageBytes = std::span<Byte>(data).subspan(DatabaseHeader::kSerializedSize, kDefaultPageSize);
  Page page;
  page.deserialize(pageBytes);
  assert(page.header().pageId == kHeaderMirrorPageId && page.header().usedBytes == WalIdentity::kSize);
  std::size_t pageIdentityVersion = sizeof(std::uint32_t);
  binary::writeLittleEndian(page.payload(), pageIdentityVersion, WalIdentity::kVersion + 1);
  rewriteIdentityChecksum(page.payload());
  const auto invalidPage = page.serialize();
  std::ranges::copy(invalidPage, pageBytes.begin());
  writeFixture(fixture, data);
  assertOpenPreservesFilesOnRejection(fixture);
  removeTestDatabaseArtifacts(fixture);
}

void checkMalformedFrames(const std::filesystem::path& source, const std::filesystem::path& directory) {
  const auto originalWal = readFixture(databaseWalPath(source));
  const auto fixture = directory / "bad-frame.zdb";
  copyPair(source, fixture);
  std::uint64_t lastLsn = 0;
  {
    LogManager log(databaseWalPath(source));
    lastLsn = log.replay().back().lsn().value();
  }
  const auto nextLsn = LogSequenceNumber{lastLsn + 1};
  const auto complete = LogRecord{nextLsn, LogRecordType::Checkpoint, {}}.serialize();
  auto wrongMagic = complete;
  wrongMagic.front() ^= 0x01;
  const auto wrongType = LogRecord{nextLsn, static_cast<LogRecordType>(0xFF), {}}.serialize();
  const auto repeatedLsn = LogRecord{LogSequenceNumber{lastLsn}, LogRecordType::Checkpoint, {}}.serialize();
  const auto invalidCheckpoint = LogRecord{nextLsn, LogRecordType::Checkpoint, {0x01}}.serialize();
  for (const auto& frame : {wrongMagic, wrongType, repeatedLsn, invalidCheckpoint}) {
    auto bytes = originalWal;
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    bytes.insert(bytes.end(), complete.begin(), complete.begin() + 3);
    writeFixture(databaseWalPath(fixture), bytes);
    assertOpenPreservesFilesOnRejection(fixture);
  }
  removeTestDatabaseArtifacts(fixture);
}

void assertBoundTruncationRejected(const std::filesystem::path& wal) {
  const auto before = readFixture(wal);
  bool rejected = false;
  {
    LogManager log(wal);
    try { log.truncate(LogSequenceNumber{1}); }
    catch (const std::exception&) { rejected = true; }
  }
  assert(rejected);
  assert(readFixture(wal) == before);
}

void checkLegacyMigration(const std::filesystem::path& directory) {
  const auto legacy = directory / "initialized-legacy.zdb";
  std::array<Byte, DatabaseHeader::kUuidSize> legacyUuid{};
  {
    FileManager files;
    files.createDatabase(legacy);
    legacyUuid = files.header().databaseUuid;
    LogManager log(databaseWalPath(legacy), files.ownership());
    TransactionManager transactions(log);
    StorageMutationCoordinator mutations(files, log, transactions);
    FreePageManager freePages(files, mutations);
    MetadataManager metadata(files, freePages, mutations);
    metadata.saveMetadata();
  }
  {
    DatabaseRuntime runtime(legacy);
    (void)runtime.createTable("records");
    runtime.records().put("records", "key", kValue);
  }
  const auto legacyData = readFixture(legacy);
  const auto legacyWal = readFixture(databaseWalPath(legacy));
  const auto migratedDirectory = directory / "migrated";
  const auto migrated = migratedDirectory / legacy.filename();
  DatabaseRuntime::snapshotTo(legacy, migratedDirectory);
  assert(readFixture(legacy) == legacyData);
  assert(readFixture(databaseWalPath(legacy)) == legacyWal);
  const auto identity = identityOf(migrated);
  assert(identity.databaseUuid == legacyUuid);
  assert(identity.snapshotLsn > 0);
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime runtime(migrated);
    assert(runtime.records().get("records", "key") == kValue);
  }
  removeTestDatabaseArtifacts(migrated);
  std::filesystem::remove(migratedDirectory);
  removeTestDatabaseArtifacts(legacy);
}

void checkNonPageWal(const std::filesystem::path& database) {
  {
    FileManager files;
    files.createDatabase(database, true);
    assert(files.walIdentity().has_value());
  }
  const auto wal = databaseWalPath(database);
  {
    LogManager log(wal);
    (void)log.append(LogRecordType::DatabaseCreated);
    (void)log.append(LogRecordType::DatabaseOpened);
    const auto transaction = LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = TransactionId{1}});
    (void)log.append(LogRecordType::TransactionStarted, transaction);
    const auto metadata = LogPayloadCodec::encodeMetadataUpdated(MetadataUpdatedPayload{
      .kind = MetadataPayloadKind::DatabaseMetadata,
      .pageId = kDatabaseMetadataPageId,
      .metadataVersion = 1,
      .payloadChecksum = 0,
    });
    (void)log.append(LogRecordType::MetadataUpdated, metadata);
    (void)log.append(LogRecordType::TransactionCommitted, transaction);
    (void)log.checkpoint();
  }
  const auto originalWal = readFixture(wal);
  const auto identity = identityOf(database);
  assert(identity.snapshotLsn == 0);
  assert(identityOf(database) == identity);
  assert(readFixture(wal) == originalWal);
}

} // namespace

void test_snapshot_generation() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_snapshot_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto source = directory / "source.zdb";
  const auto other = directory / "other.zdb";
  const auto legacy = directory / "legacy.zdb";
  const auto swap = directory / "swap.zdb";
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "key", kValue);
  }
  checkNonPageWal(other);
  {
    FileManager files;
    files.createDatabase(legacy);
    assert(!files.walIdentity().has_value());
  }
  {
    FileManager files;
    files.openDatabase(legacy);
    assert(!files.walIdentity().has_value());
  }
  const auto sourceIdentity = identityOf(source);
  assert(sourceIdentity.snapshotLsn == 0);
  assert(sourceIdentity.databaseUuid != identityOf(other).databaseUuid);
  copyPair(source, swap);
  writeFixture(databaseWalPath(swap), readFixture(databaseWalPath(other)));
  assertOpenPreservesFilesOnRejection(swap);
  writeFixture(databaseWalPath(swap), readFixture(databaseWalPath(legacy)));
  assertOpenPreservesFilesOnRejection(swap);
  writeFixture(swap, readFixture(legacy));
  writeFixture(databaseWalPath(swap), readFixture(databaseWalPath(source)));
  assertOpenPreservesFilesOnRejection(swap);
  removeTestDatabaseArtifacts(swap);

  checkTornTails(source, other, directory);
  checkMalformedIdentities(source, directory);
  checkMalformedFrames(source, directory);
  assertBoundTruncationRejected(databaseWalPath(source));
  checkLegacyMigration(directory);

  // Put a durable marker after the last page change so testing the snapshot floor
  // does not rely on page LSNs to reject an older complete WAL.
  {
    LogManager log(databaseWalPath(source));
    (void)log.checkpoint();
  }
  const auto originalData = readFixture(source);
  const auto originalWal = readFixture(databaseWalPath(source));
  const auto snapshotDirectory = directory / "copy";
  const auto snapshot = snapshotDirectory / source.filename();
  DatabaseRuntime::snapshotTo(source, snapshotDirectory);
  assert(readFixture(source) == originalData);
  assert(readFixture(databaseWalPath(source)) == originalWal);
  const auto snapshotIdentity = identityOf(snapshot);
  assert(snapshotIdentity.databaseUuid == sourceIdentity.databaseUuid);
  assert(snapshotIdentity.generation != sourceIdentity.generation);
  assert(snapshotIdentity.snapshotLsn > sourceIdentity.snapshotLsn);
  const auto snapshotWal = readFixture(databaseWalPath(snapshot));
  const auto siblingDirectory = directory / "sibling";
  const auto sibling = siblingDirectory / source.filename();
  DatabaseRuntime::snapshotTo(source, siblingDirectory);
  const auto siblingIdentity = identityOf(sibling);
  assert(siblingIdentity.databaseUuid == snapshotIdentity.databaseUuid);
  assert(siblingIdentity.generation != snapshotIdentity.generation);
  assert(siblingIdentity.snapshotLsn == snapshotIdentity.snapshotLsn);
  assert(readFixture(source) == originalData);
  assert(readFixture(databaseWalPath(source)) == originalWal);
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime runtime(snapshot);
    assert(runtime.records().get("records", "key") == kValue);
    assert(identityOf(source) == sourceIdentity);
  }

  copyPair(snapshot, swap);
  writeFixture(databaseWalPath(swap), originalWal);
  assertOpenPreservesFilesOnRejection(swap);
  writeFixture(databaseWalPath(swap), readFixture(databaseWalPath(sibling)));
  assertOpenPreservesFilesOnRejection(swap);
  auto oldWalSize = WalIdentity::kSize;
  bool floorRecordSeen = false;
  {
    LogManager log(databaseWalPath(snapshot));
    for (const auto& record : log.replay()) {
      if (record.lsn().value() >= snapshotIdentity.snapshotLsn) {
        floorRecordSeen = record.lsn().value() == snapshotIdentity.snapshotLsn;
        break;
      }
      oldWalSize += record.serialize().size();
    }
  }
  assert(floorRecordSeen && oldWalSize < snapshotWal.size());
  writeFixture(databaseWalPath(swap),
               std::span<const Byte>(snapshotWal).first(oldWalSize));
  assertOpenPreservesFilesOnRejection(swap, "snapshot LSN");
  removeTestDatabaseArtifacts(swap);
  for (const auto& database : {source, other, legacy, snapshot, sibling}) removeTestDatabaseArtifacts(database);
  std::filesystem::remove(snapshotDirectory);
  std::filesystem::remove(siblingDirectory);
  std::filesystem::remove(directory);
}
