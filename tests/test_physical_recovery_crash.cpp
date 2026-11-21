#include "core/DatabaseRuntime.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogPayload.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

using namespace zenthrildb;

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
namespace {
constexpr int kInjectedCrashCode = 91;
constexpr auto kChildTimeout = std::chrono::seconds(30);
using RawPage = std::array<Byte, kDefaultPageSize>;

std::streamoff pageOffset(PageId id) {
  return static_cast<std::streamoff>(DatabaseHeader::serializedSize()) +
    static_cast<std::streamoff>(id) * kDefaultPageSize;
}

RawPage readRawPage(const std::filesystem::path& database, PageId id) {
  RawPage image{};
  std::ifstream input(database, std::ios::binary);
  input.seekg(pageOffset(id));
  input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
  if (!input) throw std::runtime_error("Failed to read physical recovery fixture page");
  return image;
}

void writeRawPage(const std::filesystem::path& database, PageId id, const RawPage& image) {
  std::fstream output(database, std::ios::binary | std::ios::in | std::ios::out);
  output.seekp(pageOffset(id));
  output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  output.close();
  if (!output) throw std::runtime_error("Failed to simulate a damaged fixture page");
}

void runCrashProcess(const char* executablePath, const std::filesystem::path& database) {
#ifdef _WIN32
  (void)executablePath;
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size()) {
    throw std::runtime_error("Failed to determine the physical recovery test executable path");
  }
  executable.resize(length);
  const auto workingDirectory = database.parent_path();
  auto command = L"\"" + executable + L"\" --physical-recovery-crash-child \"" +
    database.filename().wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, workingDirectory.c_str(), &startup, &process)) {
    throw std::runtime_error("Failed to start physical recovery crash child");
  }
  CloseHandle(process.hThread);
  const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kChildTimeout);
  if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 3);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Physical recovery crash child did not exit");
  }
  DWORD exitCode{};
  const auto receivedStatus = GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  if (!receivedStatus || exitCode != kInjectedCrashCode) {
    throw std::runtime_error("Physical recovery child missed the repair crash barrier");
  }
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Failed to fork physical recovery crash child");
  if (child == 0) {
    execl(executablePath, executablePath, "--physical-recovery-crash-child", database.c_str(), nullptr);
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
  int status{};
  for (;;) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) throw std::runtime_error("Failed to wait for physical recovery crash child");
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Physical recovery crash child did not exit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != kInjectedCrashCode) {
    throw std::runtime_error("Physical recovery child missed the repair crash barrier");
  }
#endif
}
std::string readFixtureBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read snapshot recovery fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

RawPage stageSnapshotMutation(const std::filesystem::path& database, PageId id, bool commit) {
  FileManager files;
  files.openDatabase(database);
  LogManager log(databaseWalPath(database), files.header(), files.walIdentity(), false, files.ownership());
  TransactionManager transactions(log);
  const auto transaction = transactions.begin();
  const auto before = files.readPage(id);
  auto after = before;
  assert(after.header().pageType == PageType::Overflow && after.header().usedBytes > 0);
  after.payload()[after.header().usedBytes - 1] = 10;
  (void)log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber lsn) {
    after.setPageLsn(lsn.value());
    return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
      .transactionId = transaction.id(),
      .pageId = id,
      .previousTxLsn = 0,
      .pageFormatVersion = Page::kFormatVersionV2,
      .beforeImage = before.serialize(),
      .afterImage = after.serialize(),
    });
  });
  if (commit) {
    transactions.commit(transaction.id());
    return after.serialize();
  }
  log.flush();
  files.writePage(after);
  files.flush();
  return before.serialize();
}

void checkSnapshotRecoveryCrashes(const char* executablePath, const std::filesystem::path& directory,
                                 bool reduceHistory = false) {
  const auto source = directory / "snapshot-source.zdb";
  const auto published = directory / "baseline";
  const std::vector<Byte> confirmed{7, 8, 9};
  const std::vector<Byte> untouched{10, 11, 12};
  constexpr auto changedId = kFirstUserPageId + 1;
  constexpr auto untouchedId = kFirstUserPageId + 2;
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "confirmed", confirmed);
    runtime.records().put("records", "untouched", untouched);
  }
  const auto sourceData = readFixtureBytes(source);
  const auto sourceWal = readFixtureBytes(databaseWalPath(source));
  if (reduceHistory) DatabaseRuntime::reduceWalTo(source, published);
  else DatabaseRuntime::snapshotTo(source, published, true);
  const auto snapshot = published / source.filename();
  assert(DatabaseRuntime::verifySnapshot(snapshot).identity.baselineRequired);
  assert(DatabaseRuntime::verifySnapshot(snapshot).identity.reducedHistory == reduceHistory);
  for (const std::string mode : {"baseline", "redo", "undo"}) {
    const auto database = directory / (mode + ".zdb");
    std::filesystem::copy_file(snapshot, database);
    std::filesystem::copy_file(databaseWalPath(snapshot), databaseWalPath(database));
    std::filesystem::copy_file(snapshotBaselinePath(snapshot), snapshotBaselinePath(database));
    std::filesystem::copy_file(snapshotManifestPath(snapshot), snapshotManifestPath(database));
    auto expected = readFixtureBytes(database);
    const auto expectedPage = mode == "baseline" ? readRawPage(database, changedId)
      : stageSnapshotMutation(database, changedId, mode == "redo");
    expected.replace(static_cast<std::size_t>(pageOffset(changedId)), kDefaultPageSize,
                     reinterpret_cast<const char*>(expectedPage.data()), expectedPage.size());
    const auto originalUntouched = readRawPage(database, untouchedId);
    const auto wal = readFixtureBytes(databaseWalPath(database));
    const auto baseline = readFixtureBytes(snapshotBaselinePath(database));
    const auto manifest = readFixtureBytes(snapshotManifestPath(database));
    auto damaged = readRawPage(database, changedId);
    damaged[PageHeader::serializedSize()] ^= 1;
    writeRawPage(database, changedId, damaged);
    auto damagedUntouched = originalUntouched;
    damagedUntouched[PageHeader::serializedSize()] ^= 1;
    writeRawPage(database, untouchedId, damagedUntouched);

    runCrashProcess(executablePath, database);
    assert(readRawPage(database, changedId) == expectedPage);
    assert(readRawPage(database, untouchedId) == damagedUntouched);
    assert(readFixtureBytes(databaseWalPath(database)) == wal);
    runCrashProcess(executablePath, database);
    assert(readFixtureBytes(database) == expected);
    assert(readRawPage(database, untouchedId) == originalUntouched);
    for (int reopen = 0; reopen < 2; ++reopen) {
      DatabaseRuntime runtime(database);
      const auto expectedValue = mode == "redo" ? std::vector<Byte>{7, 8, 10} : confirmed;
      assert(runtime.records().get("records", "confirmed") == expectedValue);
      assert(runtime.records().get("records", "untouched") == untouched);
      (void)runtime.checkConsistency();
    }
    assert(DatabaseRuntime::verifySnapshot(database).identity.baselineRequired);
    assert(readFixtureBytes(database) == expected);
    assert(readFixtureBytes(databaseWalPath(database)) == wal);
    assert(readFixtureBytes(snapshotBaselinePath(database)) == baseline);
    assert(readFixtureBytes(snapshotManifestPath(database)) == manifest);
    removeTestDatabaseArtifacts(database);
  }
  assert(readFixtureBytes(source) == sourceData);
  assert(readFixtureBytes(databaseWalPath(source)) == sourceWal);
  removeTestDatabaseArtifacts(snapshot);
  std::filesystem::remove(published);
  removeTestDatabaseArtifacts(source);
}

} // namespace
#endif

void test_reduced_recovery_crash(const char* executablePath) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_reduced_recovery_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(directory));
  checkSnapshotRecoveryCrashes(executablePath, directory, true);
  std::filesystem::remove(directory);
#else
  (void)executablePath;
#endif
}
int runPhysicalRecoveryCrashChild(const char* path) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  IoFaultInjector::armCrash(IoFaultPoint::RecoveryPageRepair);
  DatabaseRuntime runtime{std::filesystem::path(path)};
#else
  (void)path;
#endif
  return 2;
}

void test_physical_recovery_crash(const char* executablePath) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_physical_recovery_crash_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto database = directory / "recover.zdb";
  constexpr auto recordPageId = kFirstUserPageId + 1;
  const std::vector<Byte> oldValue{4, 5, 6};
  const std::vector<Byte> confirmedValue{7, 8, 9};
  {
    DatabaseRuntime runtime(database, true);
    const auto table = runtime.createTable("records");
    assert(table.rootPageId() == kFirstUserPageId);
    runtime.records().put("records", "confirmed", oldValue);
    runtime.records().put("records", "confirmed", confirmedValue);
  }
  const auto originalPage = readRawPage(database, recordPageId);
  Page recordPage;
  recordPage.deserialize(originalPage);
  assert(recordPage.header().pageType == PageType::Overflow);
  auto damagedPage = originalPage;
  damagedPage[PageHeader::serializedSize()] ^= 1;
  writeRawPage(database, recordPageId, damagedPage);

  runCrashProcess(executablePath, database);
  assert(readRawPage(database, recordPageId) == originalPage);
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime recovered(database);
    assert(recovered.records().get("records", "confirmed") == confirmedValue);
    assert(recovered.checkConsistency().pagesChecked > recordPageId);
  }
  assert(readRawPage(database, recordPageId) == originalPage);
  checkSnapshotRecoveryCrashes(executablePath, directory);
  removeTestDatabaseArtifacts(database);
  std::filesystem::remove(directory);
#else
  (void)executablePath;
#endif
}
