#include "core/DatabaseRuntime.hpp"
#include "storage/DatabaseMetadata.hpp"
#include "storage/PageDirectory.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogPayload.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace zenthrildb;

namespace {
std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read reduction fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(data.data(), data.size());
  output.close();
  if (!output) throw std::runtime_error("Cannot write reduction fixture");
}

std::vector<std::string> artifacts(const std::filesystem::path& path) {
  std::vector<std::string> result;
  for (const auto& file : {path, databaseWalPath(path), snapshotBaselinePath(path), snapshotManifestPath(path)}) {
    result.push_back(std::filesystem::exists(file) ? "present:" + bytes(file) : "missing");
  }
  return result;
}

void copyCandidate(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::filesystem::copy_file(from, to);
  std::filesystem::copy_file(databaseWalPath(from), databaseWalPath(to));
  std::filesystem::copy_file(snapshotBaselinePath(from), snapshotBaselinePath(to));
  std::filesystem::copy_file(snapshotManifestPath(from), snapshotManifestPath(to));
}

void assertEqualState(const std::filesystem::path& from, const std::filesystem::path& to) {
  const auto left = bytes(from);
  const auto right = bytes(to);
  assert(left.size() == right.size());
  assert(left.substr(0, DatabaseHeader::kSerializedSize) == right.substr(0, DatabaseHeader::kSerializedSize));
  const auto firstData = DatabaseHeader::kSerializedSize + kDefaultPageSize;
  assert(left.substr(firstData) == right.substr(firstData));
}

template <typename Operation>
void rejects(Operation operation) {
  bool rejected = false;
  try { operation(); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
}

void checkEmptyHistory(const std::filesystem::path& directory) {
  const auto source = directory / "empty.zdb";
  {
    FileManager files;
    files.createDatabase(source);
    auto metadata = files.readPage(kDatabaseMetadataPageId);
    const auto metadataBytes = DatabaseMetadata{}.serialize();
    std::copy(metadataBytes.begin(), metadataBytes.end(), metadata.payload().begin());
    files.writePage(metadata);
    auto pageDirectory = files.readPage(kPageDirectoryPageId);
    const auto directoryBytes = PageDirectory{}.serialize();
    std::copy(directoryBytes.begin(), directoryBytes.end(), pageDirectory.payload().begin());
    files.writePage(pageDirectory);
    files.flush();
  }
  assert(bytes(databaseWalPath(source)).empty());
  const auto output = directory / "empty-candidate";
  DatabaseRuntime::reduceWalTo(source, output);
  const auto candidate = output / source.filename();
  const auto manifest = DatabaseRuntime::verifySnapshot(candidate);
  assert(manifest.identity.reducedHistory && manifest.identity.snapshotLsn == 0 && manifest.transactionHighWater == 0);
  {
    DatabaseRuntime runtime(candidate);
    assert(runtime.listTables().empty());
    (void)runtime.createTable("new");
    runtime.records().put("new", "first", std::vector<Byte>{1, 2});
    assert(runtime.records().get("new", "first") == (std::vector<Byte>{1, 2}));
    assert(runtime.checkpoint().transactionHighWater > 0);
  }
  assert(bytes(databaseWalPath(source)).empty());
  removeTestDatabaseArtifacts(candidate);
  std::filesystem::remove(output);
  removeTestDatabaseArtifacts(source);
}

void checkCorruptions(const std::filesystem::path& candidate, const std::filesystem::path& directory) {
  const auto originalWal = bytes(databaseWalPath(candidate));
  const auto raw = std::span<const Byte>(reinterpret_cast<const Byte*>(originalWal.data()), originalWal.size());
  const auto record = LogRecord::deserialize(raw.subspan(WalIdentity::kSize));
  for (int failure = 0; failure < 17; ++failure) {
    const auto copy = directory / ("corrupt-" + std::to_string(failure) + ".zdb");
    copyCandidate(candidate, copy);
    auto wal = originalWal;
    if (failure >= 10) {
      if (failure == 10 || failure == 11) {
        const auto path = failure == 10 ? snapshotBaselinePath(copy) : snapshotManifestPath(copy);
        auto data = bytes(path);
        data.back() ^= 1;
        writeBytes(path, data);
      } else if (failure == 12 || failure == 13) {
        std::filesystem::remove(snapshotBaselinePath(copy));
        if (failure == 13) std::filesystem::remove(snapshotManifestPath(copy));
      } else if (failure == 14 || failure == 15) {
        auto identity = DatabaseRuntime::verifySnapshot(candidate).identity;
        if (failure == 14) identity.databaseUuid[0] ^= 1;
        else identity.generation[0] ^= 1;
        const auto prefix = identity.serialize();
        wal.replace(0, prefix.size(), reinterpret_cast<const char*>(prefix.data()), prefix.size());
      } else {
        std::filesystem::remove(databaseWalPath(copy));
      }
    } else if (failure == 0) wal.back() ^= 1;
    else if (failure == 1) wal.resize(wal.size() - 1);
    else if (failure == 2) wal.resize(WalIdentity::kSize);
    else if (failure == 3) wal += originalWal.substr(WalIdentity::kSize);
    else {
      auto payload = LogPayloadCodec::decodeSafeCheckpoint(record.payload());
      if (failure == 4) payload.databaseUuid[0] ^= 1;
      if (failure == 5) payload.generation[0] ^= 1;
      if (failure == 6) ++payload.transactionHighWater;
      if (failure == 7) ++payload.durableWalLsn;
      const auto type = failure == 9 ? LogRecordType::Checkpoint : LogRecordType::SafeCheckpoint;
      const auto lsn = record.lsn().value() + (failure == 8 ? 1 : 0);
      const auto replacement = LogRecord{LogSequenceNumber{lsn}, type,
        type == LogRecordType::Checkpoint ? std::vector<Byte>{} : LogPayloadCodec::encodeSafeCheckpoint(payload)}.serialize();
      wal = originalWal.substr(0, WalIdentity::kSize);
      wal.append(reinterpret_cast<const char*>(replacement.data()), replacement.size());
    }
    if (failure < 10 || failure == 14 || failure == 15) writeBytes(databaseWalPath(copy), wal);
    const auto before = artifacts(copy);
    rejects([&] { DatabaseRuntime runtime(copy); });
    rejects([&] { (void)DatabaseRuntime::verifySnapshot(copy); });
    assert(artifacts(copy) == before);
    const auto destination = directory / ("rejected-" + std::to_string(failure));
    rejects([&] { DatabaseRuntime::reduceWalTo(copy, destination); });
    assert(!std::filesystem::exists(destination) && artifacts(copy) == before);
    removeTestDatabaseArtifacts(copy);
  }
}
} // namespace

void test_wal_reduction() {
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_reduction_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(directory));
  checkEmptyHistory(directory);
  const auto source = directory / "source.zdb";
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    for (int i = 0; i < 8; ++i) runtime.records().put("records", std::to_string(i), std::vector<Byte>{1, 2, 3});
    runtime.records().put("records", "deleted", std::vector<Byte>{4});
    assert(runtime.records().erase("records", "deleted"));
    (void)runtime.checkpoint();
  }
  // A durable BEGIN without COMMIT still contributes to the preserved ID floor.
  {
    FileManager files;
    files.openDatabase(source);
    LogManager wal(databaseWalPath(source), files.header(), files.walIdentity(), false, files.ownership());
    TransactionManager transactions(wal);
    (void)transactions.begin();
  }
  const auto originalData = bytes(source);
  const auto originalWal = bytes(databaseWalPath(source));
  {
    DatabaseRuntime busy(source);
    rejects([&] { DatabaseRuntime::reduceWalTo(source, directory / "busy"); });
    assert(!std::filesystem::exists(directory / "busy"));
  }
  for (bool required : {false, true}) {
    const auto snapshotDirectory = directory / (required ? "v2" : "v1");
    DatabaseRuntime::snapshotTo(source, snapshotDirectory, required);
    const auto snapshot = snapshotDirectory / source.filename();
    const auto before = artifacts(snapshot);
    const auto reducedDirectory = directory / (required ? "v2-reduced" : "v1-reduced");
    DatabaseRuntime::reduceWalTo(snapshot, reducedDirectory);
    const auto reduced = reducedDirectory / source.filename();
    assert(DatabaseRuntime::verifySnapshot(reduced).identity.reducedHistory);
    assertEqualState(snapshot, reduced);
    assert(artifacts(snapshot) == before);
    removeTestDatabaseArtifacts(reduced);
    std::filesystem::remove(reducedDirectory);
    removeTestDatabaseArtifacts(snapshot);
    std::filesystem::remove(snapshotDirectory);
  }
  const auto output = directory / "candidate";
  DatabaseRuntime::reduceWalTo(source, output);
  const auto candidate = output / source.filename();
  const auto manifest = DatabaseRuntime::verifySnapshot(candidate);
  assert(manifest.identity.reducedHistory && manifest.identity.baselineRequired);
  assert(manifest.transactionHighWater > 8);
  assert(bytes(databaseWalPath(candidate)).size() == WalIdentity::kSize + LogRecord::kHeaderSize +
    LogPayloadCodec::kSafeCheckpointPayloadSize);
  assert(bytes(databaseWalPath(candidate)).size() < originalWal.size());
  assertEqualState(source, candidate);
  checkCorruptions(candidate, directory);

  const auto expected = bytes(candidate);
  const auto prefixBytes = DatabaseHeader::kSerializedSize + kDefaultPageSize;
  std::filesystem::resize_file(candidate, prefixBytes);
  for (int reopen = 0; reopen < 2; ++reopen) {
    DatabaseRuntime runtime(candidate);
    assert(runtime.records().scan("records").size() == 8);
    assert(!runtime.records().get("records", "deleted"));
    assert(runtime.records().get("records", "0") == (std::vector<Byte>{1, 2, 3}));
    assert(runtime.checkpoint().transactionHighWater == manifest.transactionHighWater);
  }
  assert(bytes(candidate) == expected);
  {
    DatabaseRuntime runtime(candidate);
    runtime.records().put("records", "later", std::vector<Byte>{5});
    assert(runtime.checkpoint().transactionHighWater > manifest.transactionHighWater);
    runtime.backupTo(directory / "backup");
  }
  const auto second = directory / "second";
  DatabaseRuntime::reduceWalTo(candidate, second);
  const auto next = second / source.filename();
  const auto nextManifest = DatabaseRuntime::verifySnapshot(next);
  assert(nextManifest.identity.snapshotLsn > manifest.identity.snapshotLsn);
  assert(nextManifest.transactionHighWater > manifest.transactionHighWater);
  assertEqualState(candidate, next);
  {
    DatabaseRuntime runtime(next);
    assert(runtime.records().get("records", "later") == (std::vector<Byte>{5}));
  }
  const auto child = directory / "child";
  DatabaseRuntime::snapshotTo(next, child);
  assert(DatabaseRuntime::verifySnapshot(child / source.filename()).identity.reducedHistory);
  const auto before = artifacts(candidate);
  rejects([&] { DatabaseRuntime::reduceWalTo(candidate, output); });
  assert(artifacts(candidate) == before);
  assert(bytes(source) == originalData && bytes(databaseWalPath(source)) == originalWal);
  for (const auto& folder : {output, second, child, directory / "backup"}) {
    removeTestDatabaseArtifacts(folder / source.filename());
    std::filesystem::remove(folder);
  }
  removeTestDatabaseArtifacts(source);
  std::filesystem::remove(directory);
}
