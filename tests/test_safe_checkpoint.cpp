#include "core/DatabaseRuntime.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogPayload.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace zenthrildb;

namespace {

const std::vector<Byte> kValue{0x11, 0, 0xB6};

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read checkpoint test fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

WalIdentity readIdentity(const std::filesystem::path& database) {
  std::ifstream file(databaseWalPath(database), std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read checkpoint test WAL");
  const auto identity = readWalIdentity(file);
  assert(identity.has_value());
  return *identity;
}

void assertValidCheckpoints(const std::filesystem::path& database, std::size_t expected) {
  const auto identity = readIdentity(database);
  LogManager reader(databaseWalPath(database));
  std::uint64_t previousLsn = 0;
  std::uint64_t transactionHighWater = 0;
  std::size_t seen = 0;
  for (const auto& record : reader.replay()) {
    if (record.type() == LogRecordType::TransactionStarted) {
      const auto tx = LogPayloadCodec::decodeTransaction(record.payload());
      transactionHighWater = std::max(transactionHighWater, tx.transactionId.value());
    }
    if (record.type() == LogRecordType::SafeCheckpoint) {
      const auto payload = LogPayloadCodec::decodeSafeCheckpoint(record.payload());
      assert(payload.databaseUuid == identity.databaseUuid);
      assert(payload.durableWalLsn == previousLsn);
      assert(payload.transactionHighWater == transactionHighWater);
      if (record.lsn().value() > identity.snapshotLsn) {
        assert(payload.generation == identity.generation);
      }
      ++seen;
    }
    previousLsn = record.lsn().value();
  }
  assert(seen == expected);
}

void checkEmptyCheckpoint(const std::filesystem::path& path) {
  {
    DatabaseRuntime database(path, true);
    assert(database.listTables().empty());
    const auto dataBefore = readBytes(path);
    const auto walPath = databaseWalPath(path);
    const auto walBefore = readBytes(walPath);
    const auto result = database.checkpoint();
    assert(result.durableWalLsn < result.checkpointLsn);
    assert(readBytes(path) == dataBefore);
    assert(readBytes(walPath).starts_with(walBefore));
  }
  assertValidCheckpoints(path, 1);
  {
    DatabaseRuntime database(path);
    assert(database.listTables().empty());
    (void)database.checkConsistency();
  }
  removeTestDatabaseArtifacts(path);
}

void checkAbandonedTransaction(const std::filesystem::path& path) {
  {
    DatabaseRuntime database(path, true);
    (void)database.createTable("records");
    database.records().put("records", "durable", kValue);
  }
  TransactionId abandoned;
  LogSequenceNumber beginLsn;
  {
    LogManager log(databaseWalPath(path));
    TransactionManager transactions(log);
    abandoned = transactions.begin().id();
    const auto records = log.replay();
    assert(records.back().type() == LogRecordType::TransactionStarted);
    beginLsn = records.back().lsn();
  }
  {
    DatabaseRuntime database(path);
    assert(database.records().get("records", "durable") == kValue);
    const auto checkpoint = database.checkpoint();
    assert(checkpoint.durableWalLsn == beginLsn);
    assert(checkpoint.transactionHighWater == abandoned.value());
    database.records().put("records", "next", kValue);
  }
  {
    LogManager log(databaseWalPath(path));
    const auto records = log.replay();
    auto lastBegin = TransactionId{};
    for (const auto& record : records) {
      if (record.type() == LogRecordType::TransactionStarted) {
        lastBegin = LogPayloadCodec::decodeTransaction(record.payload()).transactionId;
      }
    }
    assert(lastBegin.value() > abandoned.value());
  }
  assertValidCheckpoints(path, 1);
  {
    DatabaseRuntime database(path);
    assert(database.records().get("records", "next") == kValue);
    assert(database.records().get("records", "durable") == kValue);
  }
  removeTestDatabaseArtifacts(path);
}

void checkAbandonedPageWrite(const std::filesystem::path& path) {
  {
    DatabaseRuntime database(path, true);
    (void)database.createTable("records");
    database.records().put("records", "durable", kValue);
  }
  const auto recoveredData = readBytes(path);
  const auto wal = databaseWalPath(path);
  TransactionId abandoned;
  LogSequenceNumber pageLsn;
  {
    FileManager files;
    files.openDatabase(path);
    assert(files.header().totalPages == kFirstUserPageId + 2);
    const auto before = files.readPage(kFirstUserPageId + 1);
    auto after = before;
    assert(before.header().pageType == PageType::Overflow && before.header().usedBytes > 0);
    after.payload()[after.header().usedBytes - 1] ^= 0x20;
    LogManager log(wal, files.header(), files.walIdentity());
    TransactionManager transactions(log);
    abandoned = transactions.begin().id();
    pageLsn = log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber assigned) {
      after.setPageLsn(assigned.value());
      return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
        .transactionId = abandoned,
        .pageId = before.header().pageId,
        .previousTxLsn = 0,
        .pageFormatVersion = Page::kFormatVersionV2,
        .beforeImage = before.serialize(),
        .afterImage = after.serialize(),
      });
    });
    assert(after.pageLsn() == pageLsn.value());
    files.writePage(after);
    files.flush();
  }
  assert(readBytes(path) != recoveredData);
  {
    DatabaseRuntime database(path);
    assert(readBytes(path) == recoveredData);
    assert(database.records().get("records", "durable") == kValue);
    const auto checkpoint = database.checkpoint();
    assert(checkpoint.durableWalLsn == pageLsn);
    assert(checkpoint.transactionHighWater == abandoned.value());
  }
  assertValidCheckpoints(path, 1);
  {
    DatabaseRuntime database(path);
    assert(readBytes(path) == recoveredData);
    assert(database.records().get("records", "durable") == kValue);
    (void)database.checkConsistency();
  }
  removeTestDatabaseArtifacts(path);
}

void checkInvalidCheckpoints(const std::filesystem::path& directory) {
  const auto source = directory / "valid-checkpoint.zdb";
  CheckpointResult valid{};
  {
    DatabaseRuntime database(source, true);
    (void)database.createTable("records");
    database.records().put("records", "durable", kValue);
    valid = database.checkpoint();
  }
  const auto sourceWal = databaseWalPath(source);
  const auto dataBefore = readBytes(source);
  const auto walBefore = readBytes(sourceWal);
  const auto identity = readIdentity(source);
  const SafeCheckpointPayload expected{
    .databaseUuid = identity.databaseUuid,
    .generation = identity.generation,
    .durableWalLsn = valid.checkpointLsn.value(),
    .transactionHighWater = valid.transactionHighWater,
  };
  std::array payloads{expected, expected, expected, expected};
  payloads[0].databaseUuid.front() ^= 0x20;
  payloads[1].generation.front() ^= 0x20;
  --payloads[2].durableWalLsn;
  ++payloads[3].transactionHighWater;
  const auto frameLsn = valid.checkpointLsn.next();
  const auto partialTail = LogRecord{frameLsn.next(), LogRecordType::Checkpoint, {}}.serialize();
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    const auto path = directory / ("invalid-checkpoint-" + std::to_string(i) + ".zdb");
    const auto wal = databaseWalPath(path);
    std::filesystem::copy_file(source, path);
    std::filesystem::copy_file(sourceWal, wal);
    const auto bytes = LogRecord{frameLsn, LogRecordType::SafeCheckpoint,
                                 LogPayloadCodec::encodeSafeCheckpoint(payloads[i])}.serialize();
    {
      std::ofstream output(wal, std::ios::binary | std::ios::app);
      output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.write(reinterpret_cast<const char*>(partialTail.data()), 3);
      output.close();
      assert(output);
    }
    const auto invalidWal = readBytes(wal);
    assert(invalidWal.starts_with(walBefore));
    bool rejected = false;
    try { DatabaseRuntime database(path); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
    assert(readBytes(path) == dataBefore);
    assert(readBytes(wal) == invalidWal);
    removeTestDatabaseArtifacts(path);
  }
  removeTestDatabaseArtifacts(source);
}

void checkRepeatableCheckpoints(const std::filesystem::path& path) {
  DatabaseRuntime database(path, true);
  (void)database.createTable("records");
  for (std::size_t i = 0; i < 20; ++i) {
    const auto key = "record-" + std::to_string(i);
    database.records().put("records", key, kValue);
    database.records().put("records", key, kValue);
  }
  const auto wal = databaseWalPath(path);
  const auto dataBefore = readBytes(path);
  const auto walBefore = readBytes(wal);
  const auto first = database.checkpoint();
  const auto walAfterFirst = readBytes(wal);
  assert(walAfterFirst.starts_with(walBefore) && walAfterFirst.size() > walBefore.size());
  assert(readBytes(path) == dataBefore);
  assert(first.durableWalLsn < first.checkpointLsn);
  assert(first.transactionHighWater > 0);

  const auto second = database.checkpoint();
  const auto walAfterSecond = readBytes(wal);
  assert(walAfterSecond.starts_with(walAfterFirst) && walAfterSecond.size() > walAfterFirst.size());
  assert(readBytes(path) == dataBefore);
  assert(second.durableWalLsn == first.checkpointLsn);
  assert(first.checkpointLsn < second.checkpointLsn);
  assert(first.transactionHighWater == second.transactionHighWater);
  assertValidCheckpoints(path, 2);
}

void checkConcurrentCheckpoints(const std::filesystem::path& path) {
  {
    DatabaseRuntime database(path, true);
    (void)database.createTable("records");
    std::array<std::exception_ptr, 3> errors{};
    std::atomic<bool> start{false};
    const auto writer = [&](std::stop_token stop, std::size_t index) {
      try {
        while (!start.load()) {
          if (stop.stop_requested()) return;
          std::this_thread::yield();
        }
        for (std::size_t i = 0; i < 10; ++i) {
          const auto key = std::to_string(index) + "-" + std::to_string(i);
          database.records().put("records", key, kValue);
        }
      } catch (...) {
        errors[index] = std::current_exception();
      }
    };
    std::jthread first(writer, 0);
    std::jthread second(writer, 1);
    std::jthread checkpointer([&](std::stop_token stop) {
      try {
        while (!start.load()) {
          if (stop.stop_requested()) return;
          std::this_thread::yield();
        }
        for (std::size_t i = 0; i < 10; ++i) (void)database.checkpoint();
      } catch (...) {
        errors[2] = std::current_exception();
      }
    });
    start.store(true);
    first.join();
    second.join();
    checkpointer.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    assert(database.records().scan("records").size() == 20);
  }
  assertValidCheckpoints(path, 10);
  DatabaseRuntime database(path);
  assert(database.records().scan("records").size() == 20);
  (void)database.checkConsistency();
}

void checkSnapshotAndRepair(const std::filesystem::path& source, const std::filesystem::path& directory) {
  const auto snapshotDirectory = directory / "snapshot";
  const auto snapshot = snapshotDirectory / source.filename();
  const auto sourceIdentity = readIdentity(source);
  const auto sourceWal = readBytes(databaseWalPath(source));
  DatabaseRuntime::snapshotTo(source, snapshotDirectory);
  const auto snapshotIdentity = readIdentity(snapshot);
  assert(snapshotIdentity.generation != sourceIdentity.generation);
  assertValidCheckpoints(snapshot, 2);
  {
    DatabaseRuntime database(snapshot);
    const auto checkpoint = database.checkpoint();
    assert(checkpoint.durableWalLsn.value() == snapshotIdentity.snapshotLsn);
    assert(database.records().get("records", "record-0") == kValue);
  }
  assertValidCheckpoints(snapshot, 3);
  const auto goodData = readBytes(snapshot);
  const auto goodWal = readBytes(databaseWalPath(snapshot));
  {
    // Flip a byte in the last live page. Recovery still needs the history retained
    // before the snapshot boundary and both generations of checkpoint records.
    std::fstream file(snapshot, std::ios::binary | std::ios::in | std::ios::out);
    const auto offset = static_cast<std::streamoff>(goodData.size() - 17);
    file.seekp(offset);
    file.put(static_cast<char>(goodData[goodData.size() - 17] ^ 0x20));
    file.close();
    assert(file);
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    DatabaseRuntime database(snapshot);
    assert(database.records().get("records", "record-19") == kValue);
    assert(database.records().scan("records").size() == 20);
    (void)database.checkConsistency();
  }
  assert(readBytes(snapshot) == goodData);
  assert(readBytes(databaseWalPath(snapshot)) == goodWal);
  assert(readBytes(databaseWalPath(source)) == sourceWal);
  removeTestDatabaseArtifacts(snapshot);
  std::filesystem::remove(snapshotDirectory);
}

void checkLegacyCheckpointRejected(const std::filesystem::path& path) {
  {
    FileManager files;
    files.createDatabase(path);
    LogManager log(databaseWalPath(path));
    TransactionManager transactions(log);
    StorageMutationCoordinator mutations(files, log, transactions);
    FreePageManager freePages(files, mutations);
    MetadataManager metadata(files, freePages, mutations);
    metadata.saveMetadata();
  }
  const auto wal = databaseWalPath(path);
  const auto dataBefore = readBytes(path);
  const auto walBefore = readBytes(wal);
  {
    DatabaseRuntime database(path);
    bool rejected = false;
    try { (void)database.checkpoint(); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
  }
  assert(readBytes(path) == dataBefore);
  assert(readBytes(wal) == walBefore);
  removeTestDatabaseArtifacts(path);
}

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
void checkCheckpointFailures(const std::filesystem::path& directory) {
  struct Failure {
    const char* name;
    IoFaultPoint point;
    std::size_t occurrence;
    bool beforeAppend;
  };
  constexpr std::array failures{
    Failure{"wal-sync-before", IoFaultPoint::WalSync, 1, true},
    Failure{"wal-sync-after", IoFaultPoint::WalSync, 2, false},
    Failure{"page-sync", IoFaultPoint::PageSync, 1, true},
    Failure{"wal-write", IoFaultPoint::WalWrite, 1, true},
    Failure{"wal-partial-write", IoFaultPoint::WalPartialWrite, 1, false},
    Failure{"no-space", IoFaultPoint::NoSpace, 1, true},
  };
  for (const auto& failure : failures) {
    const auto path = directory / (std::string(failure.name) + ".zdb");
    const auto wal = databaseWalPath(path);
    std::string dataBefore;
    std::string walBefore;
    {
      DatabaseRuntime database(path, true);
      (void)database.createTable("records");
      database.records().put("records", "durable", kValue);
      (void)database.checkpoint();
      dataBefore = readBytes(path);
      walBefore = readBytes(wal);

      IoFaultInjector::arm(failure.point, failure.occurrence);
      bool failed = false;
      try { (void)database.checkpoint(); }
      catch (const std::runtime_error&) { failed = true; }
      IoFaultInjector::clear();
      assert(failed);

      bool repeatBlocked = false;
      try { (void)database.checkpoint(); }
      catch (const std::runtime_error&) { repeatBlocked = true; }
      assert(repeatBlocked);
      bool writeBlocked = false;
      try { database.records().put("records", "blocked", kValue); }
      catch (const std::runtime_error&) { writeBlocked = true; }
      assert(writeBlocked);
    }
    assert(readBytes(path) == dataBefore);
    const auto failedWal = readBytes(wal);
    assert(failedWal.starts_with(walBefore));
    if (failure.beforeAppend) {
      assert(failedWal == walBefore);
    } else if (failure.point == IoFaultPoint::WalPartialWrite) {
      assert(failedWal.size() > walBefore.size());
      assert(failedWal.size() - walBefore.size() <
             LogRecord::kHeaderSize + LogPayloadCodec::kSafeCheckpointPayloadSize);
    }
    {
      DatabaseRuntime database(path);
      assert(database.records().get("records", "durable") == kValue);
      assert(!database.records().get("records", "blocked"));
      (void)database.checkpoint();
      database.records().put("records", "after-retry", kValue);
    }
    {
      DatabaseRuntime database(path);
      assert(database.records().get("records", "after-retry") == kValue);
      assert(!database.records().get("records", "blocked"));
      (void)database.checkConsistency();
    }
    removeTestDatabaseArtifacts(path);
  }
}
#endif

} // namespace

void test_safe_checkpoint() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_checkpoint_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto repeat = directory / "repeat.zdb";
  const auto concurrent = directory / "concurrent.zdb";
  checkEmptyCheckpoint(directory / "empty.zdb");
  checkAbandonedTransaction(directory / "abandoned.zdb");
  checkAbandonedPageWrite(directory / "abandoned-page.zdb");
  checkInvalidCheckpoints(directory);
  checkRepeatableCheckpoints(repeat);
  checkConcurrentCheckpoints(concurrent);
  checkSnapshotAndRepair(repeat, directory);
  checkLegacyCheckpointRejected(directory / "legacy.zdb");
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  checkCheckpointFailures(directory);
#endif
  removeTestDatabaseArtifacts(repeat);
  removeTestDatabaseArtifacts(concurrent);
  std::filesystem::remove(directory);
}
