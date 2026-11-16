#include "core/DatabaseRuntime.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "storage/IoFaultInjector.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace zenthrildb;

namespace {
void test_snapshot_backup() {
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_snapshot_backup_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!std::filesystem::create_directory(directory)) {
    throw std::runtime_error("Failed to create snapshot backup test directory");
  }
  const auto database = directory / "source.zdb";
  const auto snapshotDirectory = directory / "snapshot";
  const auto backupDirectory = directory / "backup";
  const auto snapshot = snapshotDirectory / database.filename();
  const auto backup = backupDirectory / database.filename();
  const std::vector<Byte> captured{2, 4, 8};
  {
    DatabaseRuntime runtime(database, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "key", std::vector<Byte>{1, 3, 5});
  }
  DatabaseRuntime::snapshotTo(database, snapshotDirectory);
  {
    DatabaseRuntime runtime(snapshot);
    runtime.records().put("records", "key", captured);
    runtime.backupTo(backupDirectory);
  }
  const auto readBytes = [](const auto& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Failed to read snapshot backup test file");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  assert(readBytes(backup) == readBytes(snapshot));
  assert(readBytes(databaseWalPath(backup)) == readBytes(databaseWalPath(snapshot)));
  assert(readBytes(snapshotBaselinePath(backup)) == readBytes(snapshotBaselinePath(snapshot)));
  assert(readBytes(snapshotManifestPath(backup)) == readBytes(snapshotManifestPath(snapshot)));
  assert(DatabaseRuntime::verifySnapshot(backup) == DatabaseRuntime::verifySnapshot(snapshot));
  {
    DatabaseRuntime restored(backup);
    assert(restored.records().get("records", "key") == captured);
    (void)restored.checkConsistency();
  }
  for (const auto& path : {snapshot, backup}) {
    removeTestDatabaseArtifacts(path);
    std::filesystem::remove(snapshotBaselinePath(path));
    std::filesystem::remove(snapshotManifestPath(path));
    std::filesystem::remove(path.parent_path());
  }
  removeTestDatabaseArtifacts(database);
  std::filesystem::remove(directory);
}

void test_directory_growth() {
  const auto path = std::filesystem::temp_directory_path() /
    ("zenthrildb_directory_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zdb");
  const auto key = [](std::size_t index) { return "key-" + std::to_string(index); };
  const std::vector<Byte> value{0, 42, 255};
  constexpr auto firstSpill = PageDirectory::kSinglePageCapacity - 1; // The table root uses one entry.
  constexpr auto records = 2 * PageDirectory::kChunkCapacity;
  {
    DatabaseRuntime runtime(path, true);
    (void)runtime.createTable("records");
    for (std::size_t i = 0; i < firstSpill; ++i) runtime.records().put("records", key(i), value);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + firstSpill + 1);
  }
  {
    DatabaseRuntime runtime(path);
    for (std::size_t i = firstSpill; i < records; ++i) runtime.records().put("records", key(i), value);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + records + 3);
  }
  {
    DatabaseRuntime runtime(path);
    assert(runtime.renameTable("records", "renamed"));
    const auto rows = runtime.records().scan("renamed");
    assert(rows.size() == records);
    for (const auto& row : rows) assert(row.second == value);
    assert(runtime.records().get("renamed", key(firstSpill)) == value);
  }
  {
    DatabaseRuntime runtime(path);
    assert(runtime.records().scan("renamed").size() == records);
    assert(runtime.dropTable("renamed"));
    (void)runtime.createTable("reused");
    runtime.records().put("reused", "test", value);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + records + 3);
  }
  {
    DatabaseRuntime runtime(path);
    assert(!runtime.tableExists("renamed"));
    assert(runtime.records().get("reused", "test") == value);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + records + 3);
  }
  removeTestDatabaseArtifacts(path);
}
} // namespace

void test_database_runtime() {
  const auto path = std::filesystem::temp_directory_path() /
    ("zenthrildb_runtime_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zdb");
  removeTestDatabaseArtifacts(path);
  auto backupDirectory = path;
  backupDirectory += ".backup";
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  for (const std::size_t failWriteAt : {1, 2, 3, 4}) {
    const auto crashPath = std::filesystem::temp_directory_path() /
      ("zenthrildb_record_fault_" + std::to_string(failWriteAt) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zdb");
    removeTestDatabaseArtifacts(crashPath);
    {
      DatabaseRuntime runtime(crashPath, true);
      (void)runtime.createTable("records");
      runtime.records().put("records", "stable", std::vector<Byte>{1, 2, 3});
      const std::vector<Byte> value{4, 5, 6};
      IoFaultInjector::arm(IoFaultPoint::PageWrite, failWriteAt);
      bool injectedFailure = false;
      try { runtime.records().put("records", "interrupted", value); }
      catch (const std::runtime_error&) { injectedFailure = true; }
      assert(injectedFailure);
    }
    {
      DatabaseRuntime recovered(crashPath);
      assert(!recovered.records().get("records", "interrupted"));
      assert(recovered.records().get("records", "stable") == (std::vector<Byte>{1, 2, 3}));
      assert(recovered.checkConsistency().pagesChecked == kFirstUserPageId + 2);
      recovered.records().put("records", "after-recovery", std::vector<Byte>{10});
    }
    {
      DatabaseRuntime recoveredAgain(crashPath);
      assert(recoveredAgain.records().get("records", "after-recovery") == (std::vector<Byte>{10}));
      assert(recoveredAgain.records().get("records", "stable") == (std::vector<Byte>{1, 2, 3}));
      assert(!recoveredAgain.records().get("records", "interrupted"));
    }
    removeTestDatabaseArtifacts(crashPath);
  }
  for (const std::size_t failWriteAt : {1, 2, 3, 4}) {
    const auto crashPath = std::filesystem::temp_directory_path() /
      ("zenthrildb_erase_fault_" + std::to_string(failWriteAt) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zdb");
    removeTestDatabaseArtifacts(crashPath);
    const std::vector<Byte> value{7, 8, 9};
    {
      DatabaseRuntime runtime(crashPath, true);
      (void)runtime.createTable("records");
      runtime.records().put("records", "preserve", value);
      IoFaultInjector::arm(IoFaultPoint::PageWrite, failWriteAt);
      bool injectedFailure = false;
      try { (void)runtime.records().erase("records", "preserve"); }
      catch (const std::runtime_error&) { injectedFailure = true; }
      assert(injectedFailure);
    }
    {
      DatabaseRuntime recovered(crashPath);
      assert(recovered.records().get("records", "preserve") == value);
      (void)recovered.checkConsistency();
    }
    removeTestDatabaseArtifacts(crashPath);
  }
#endif
  {
    DatabaseRuntime runtime(path, true);
    (void)runtime.createTable("records");
    const std::vector<Byte> firstValue{1, 2, 3};
    runtime.records().put("records", "alpha", firstValue);
    const auto firstRead = runtime.records().get("records", "alpha");
    assert(firstRead && *firstRead == firstValue);
    const std::vector<Byte> updatedValue{9, 8, 7, 6};
    runtime.records().put("records", "alpha", updatedValue);
    assert(runtime.records().scan("records").size() == 1);
    const std::vector<Byte> emptyValue{};
    runtime.records().put("records", "empty-value", emptyValue);
    assert(runtime.records().get("records", "empty-value") == emptyValue);
    (void)runtime.createTable("rename-source");
    runtime.records().put("rename-source", "rename-key", firstValue);
    assert(runtime.renameTable("rename-source", "rename-target"));
    assert(runtime.records().get("rename-target", "rename-key") == firstValue);
    assert(!runtime.records().get("records", "missing"));
    assert(!runtime.records().erase("records", "missing"));
    bool emptyKeyRejected = false;
    try { runtime.records().put("records", "", firstValue); }
    catch (const std::invalid_argument&) { emptyKeyRejected = true; }
    assert(emptyKeyRejected);
    bool oversizedValueRejected = false;
    try {
      const std::vector<Byte> tooLarge(Page::kV2PayloadSize, 0xAA);
      runtime.records().put("records", "large", tooLarge);
    } catch (const std::length_error&) { oversizedValueRejected = true; }
    assert(oversizedValueRejected);
    (void)runtime.checkConsistency();
    runtime.backupTo(backupDirectory);
#ifndef _WIN32
    const auto permissions = std::filesystem::status(backupDirectory).permissions();
    using std::filesystem::perms;
    assert((permissions & (perms::group_all | perms::others_all)) == perms::none);
#endif
    bool existingBackupRejected = false;
    try { runtime.backupTo(backupDirectory); }
    catch (const std::invalid_argument&) { existingBackupRejected = true; }
    assert(existingBackupRejected);
  }
  {
    DatabaseRuntime runtime(path);
    const std::vector<Byte> expectedValue{9, 8, 7, 6};
    assert(runtime.records().get("records", "alpha") == expectedValue);
    const std::vector<Byte> firstValue{1, 2, 3};
    assert(runtime.records().get("rename-target", "rename-key") == firstValue);
    (void)runtime.checkConsistency();
    assert(runtime.records().erase("records", "alpha"));
    assert(runtime.records().erase("records", "empty-value"));
    assert(!runtime.records().get("records", "alpha"));
    assert(runtime.records().scan("records").empty());
    runtime.records().put("records", "alpha", expectedValue);
    assert(runtime.records().get("records", "alpha") == expectedValue);
    runtime.records().put("records", "drop-me", expectedValue);
    assert(runtime.dropTable("records"));
    assert(!runtime.tableExists("records"));
    (void)runtime.checkConsistency();
  }
  const auto backupPath = backupDirectory / path.filename();
  {
    DatabaseRuntime backup(backupPath);
    assert(backup.records().get("records", "alpha") == (std::vector<Byte>{9, 8, 7, 6}));
    assert(backup.records().get("rename-target", "rename-key") == (std::vector<Byte>{1, 2, 3}));
    (void)backup.checkConsistency();
  }
  removeTestDatabaseArtifacts(backupPath);
  std::filesystem::remove(backupDirectory);
  removeTestDatabaseArtifacts(path);
  test_snapshot_backup();
  test_directory_growth();
}
