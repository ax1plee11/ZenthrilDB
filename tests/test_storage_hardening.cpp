#include "core/DatabaseRuntime.hpp"
#include "storage/SafeFilePath.hpp"
#include "storage/DurableFileSync.hpp"
#include "tests/TestDatabaseFiles.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace zenthrildb;

namespace {
std::string bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  assert(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

template <typename Operation>
void rejects(Operation operation) {
  bool rejected = false;
  try { operation(); }
  catch (const std::exception&) { rejected = true; }
  assert(rejected);
}

template <typename Operation>
void rejectsLimit(Operation operation) {
  bool rejected = false;
  try { operation(); }
  catch (const std::length_error&) { rejected = true; }
  assert(rejected);
}

void checkPaths(const std::filesystem::path& directory) {
  detail::validateStoragePath(directory.root_path(), false, true);
  const auto victim = directory / "victim";
  { std::ofstream file(victim, std::ios::binary); file << "must remain unchanged"; }
  const auto original = bytes(victim);
  const auto database = directory / "linked.zdb";
  const auto wal = databaseWalPath(database);
  for (const auto& target : {database, wal, std::filesystem::path(database.string() + ".lock"),
                            snapshotBaselinePath(database), snapshotManifestPath(database)}) {
    std::filesystem::create_hard_link(victim, target);
    rejects([&] { DatabaseRuntime runtime(database, true); });
    assert(!std::filesystem::exists(database) || target == database);
    assert(bytes(victim) == original);
    std::filesystem::remove(target);
  }
  const auto walLink = directory / "linked.wal";
  std::filesystem::create_hard_link(victim, walLink);
  rejects([&] { WriteAheadLog log(walLink); });
  rejects([&] { detail::syncFileToStorage(walLink); });
  std::filesystem::remove(walLink);

  // A stale rewrite file belongs to the caller; compaction must not delete it.
  const auto legacy = directory / "legacy.wal";
  {
    WriteAheadLog log(legacy);
    log.append(LogRecord{LogSequenceNumber{1}, LogRecordType::DatabaseOpened, {}});
    log.flush();
    const auto originalWal = bytes(legacy);
    auto rewrite = legacy;
    rewrite += ".rewrite.tmp";
    { std::ofstream file(rewrite); file << "unrelated"; }
    rejects([&] { log.truncate(LogSequenceNumber{1}); });
    assert(bytes(rewrite) == "unrelated" && bytes(legacy) == originalWal);
    std::filesystem::remove(rewrite);
  }
  std::filesystem::remove(legacy);

  const auto symlink = directory / "symbolic.zdb";
  std::error_code error;
  std::filesystem::create_symlink(victim, symlink, error);
  if (!error) {
    rejects([&] { DatabaseRuntime runtime(symlink, true); });
    rejects([&] { FileManager files; files.openDatabase(symlink); });
    std::filesystem::remove(symlink);
    std::filesystem::create_symlink(directory / "missing-target", symlink);
    rejects([&] { DatabaseRuntime runtime(symlink, true); });
    assert(!std::filesystem::exists(directory / "missing-target"));
    std::filesystem::remove(symlink);
  } else {
    std::cerr << "SKIP file symlinks: " << error.message() << '\n';
  }
  const auto alias = directory / "alias";
  error.clear();
  std::filesystem::create_directory_symlink(directory, alias, error);
  if (!error) {
    rejects([&] { DatabaseRuntime runtime(alias / "redirected.zdb", true); });
    assert(!std::filesystem::exists(directory / "redirected.zdb"));
    std::filesystem::remove(alias);
  } else {
    std::cerr << "SKIP directory symlinks: " << error.message() << '\n';
  }
#ifndef _WIN32
  const auto fifo = directory / "pipe.wal";
  assert(::mkfifo(fifo.c_str(), 0600) == 0);
  rejects([&] { WriteAheadLog log(fifo); });
  std::filesystem::remove(fifo);
#else
  rejects([&] { WriteAheadLog log(directory / "victim:stream"); });
  rejects([&] { WriteAheadLog log(directory / "NUL"); });
#endif
  assert(bytes(victim) == original);
  std::filesystem::remove(victim);
}

void checkWalLimits(const std::filesystem::path& directory) {
  const auto path = directory / "bounded.wal";
  const LogRecord first{LogSequenceNumber{1}, LogRecordType::DatabaseOpened, {1}};
  const LogRecord second{LogSequenceNumber{2}, LogRecordType::DatabaseOpened, {2}};
  const auto frameBytes = first.serialize().size();
  {
    WriteAheadLog log(path, WalLimits{frameBytes * 2, 2});
    log.append(first);
    log.append(second);
    log.flush();
    const auto before = bytes(path);
    rejectsLimit([&] { log.append(LogRecord{LogSequenceNumber{3}, LogRecordType::DatabaseOpened, {3}}); });
    assert(bytes(path) == before);
    assert(log.readAll().size() == 2);
  }
  const auto before = bytes(path);
  {
    WriteAheadLog log(path, WalLimits{frameBytes * 2, 1});
    rejectsLimit([&] { (void)log.readAll(); });
    rejectsLimit([&] { log.append(first); });
  }
  {
    WriteAheadLog log(path, WalLimits{frameBytes, 10});
    rejectsLimit([&] { (void)log.readAll(); });
  }
  assert(bytes(path) == before);
  std::filesystem::remove(path);

  // A single frame cannot cross the byte cap even if record-count space remains.
  {
    WriteAheadLog log(path, WalLimits{frameBytes, 10});
    log.append(first);
    log.flush();
    rejectsLimit([&] { log.append(second); });
    assert(log.readAll().size() == 1);
  }
  std::filesystem::remove(path);
  { std::ofstream file(path, std::ios::binary); }
  std::filesystem::resize_file(path, WalLimits{}.maxBytes + 1);
  {
    WriteAheadLog log(path);
    rejectsLimit([&] { (void)log.readAll(); });
  }
  assert(std::filesystem::file_size(path) == WalLimits{}.maxBytes + 1);
  std::filesystem::remove(path);
}

void checkDatabaseLimits(const std::filesystem::path& directory) {
  const auto path = directory / "oversized.zdb";
  {
    FileManager files;
    files.createDatabase(path);
    const auto before = bytes(path);
    Page page;
    page.header().pageId = static_cast<PageId>(kMaxDatabasePages);
    rejectsLimit([&] { files.writePage(page); });
    assert(bytes(path) == before);
  }
  std::filesystem::resize_file(path, kMaxDatabaseBytes + 1);
  rejectsLimit([&] { DatabaseRuntime runtime(path); });
  assert(std::filesystem::file_size(path) == kMaxDatabaseBytes + 1);
  removeTestDatabaseArtifacts(path);
}

void checkConcurrentRuntime(const std::filesystem::path& directory) {
  const auto path = directory / "concurrent.zdb";
  {
    DatabaseRuntime runtime(path, true);
    (void)runtime.createTable("records");
    std::atomic<bool> failed{false};
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 4; ++worker) {
      writers.emplace_back([&, worker] {
        try {
          for (int item = 0; item < 12; ++item) {
            const auto key = std::to_string(worker) + ":" + std::to_string(item);
            const std::vector<Byte> value{static_cast<Byte>(worker), static_cast<Byte>(item)};
            runtime.records().put("records", key, value);
            if (runtime.records().get("records", key) != value) failed = true;
            if (!runtime.tableExists("records") || runtime.listTables().size() != 1) failed = true;
          }
        } catch (...) { failed = true; }
      });
    }
    std::thread reader([&] {
      try {
        for (int sample = 0; sample < 12; ++sample) {
          (void)runtime.records().scan("records");
          (void)runtime.checkConsistency();
        }
      } catch (...) { failed = true; }
    });
    for (auto& writer : writers) writer.join();
    reader.join();
    assert(!failed && runtime.records().scan("records").size() == 48);
    rejects([&] { DatabaseRuntime duplicate(path); });
  }
  {
    DatabaseRuntime runtime(path);
    assert(runtime.records().scan("records").size() == 48);
    (void)runtime.checkConsistency();
  }
  removeTestDatabaseArtifacts(path);
}

void checkTrailingDestinations(const std::filesystem::path& directory) {
  const auto source = directory / "trailing.zdb";
  const auto backup = directory / "trailing-backup";
  const auto snapshot = directory / "trailing-snapshot";
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "key", std::vector<Byte>{7});
    runtime.backupTo(backup / "");
  }
  DatabaseRuntime::snapshotTo(source, snapshot / "");
  const auto copied = backup / source.filename();
  const auto captured = snapshot / source.filename();
  {
    DatabaseRuntime runtime(copied);
    assert(runtime.records().get("records", "key") == (std::vector<Byte>{7}));
  }
  (void)DatabaseRuntime::verifySnapshot(captured);
  for (const auto& path : {source, copied, captured}) {
    removeTestDatabaseArtifacts(path);
    std::filesystem::remove(snapshotBaselinePath(path));
    std::filesystem::remove(snapshotManifestPath(path));
  }
  std::filesystem::remove(backup);
  std::filesystem::remove(snapshot);
}
} // namespace

void test_storage_hardening() {
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_hardening_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(directory));
  checkPaths(directory);
  checkWalLimits(directory);
  checkDatabaseLimits(directory);
  checkConcurrentRuntime(directory);
  checkTrailingDestinations(directory);
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    // Only lock sidecars from our own rejected creation attempts may remain.
    assert(entry.path().extension() == ".lock");
    std::filesystem::remove(entry.path());
  }
  std::filesystem::remove(directory);
}
