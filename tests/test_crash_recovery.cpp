#include "transaction/TransactionManager.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "recovery/RecoveryCoordinator.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "recovery/RecoveryExecutor.hpp"
#include "recovery/PageImageRecoveryApplier.hpp"
#include "core/FileManager.hpp"
#include "core/DatabaseRuntime.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"
#include "core/StorageConsistencyChecker.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "storage/IoFaultInjector.hpp"
#include "transaction/StorageMutationCoordinator.hpp"
#include "transaction/TransactionManager.hpp"
#include "storage/Page.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <string>
#include <stdexcept>
#include <system_error>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace zenthrildb;

namespace {
std::filesystem::path uniqueTempPath(const char* suffix) {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
    (std::string("zenthrildb_process_crash_") + std::to_string(tick) + suffix);
}
#ifdef _WIN32
std::wstring quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (const auto ch : value) {
    if (ch == L'\"') result += L'\\';
    result += ch;
  }
  result += L'\"';
  return result;
}
#endif
} // namespace

int runCrashCommitChild(const char* walPath, const char* readyPath) {
  LogManager log{std::filesystem::path(walPath)};
  TransactionManager transactions(log);
  const auto transaction = transactions.begin();
  transactions.commit(transaction.id());
  (void)log.checkpoint();
  std::ofstream ready(readyPath, std::ios::binary | std::ios::trunc);
  ready << transaction.id().value();
  ready.flush();
  if (!ready) return 2;
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}


int runCrashPageChild(const char* databasePath, const char* walPath, const char* readyPath, bool commit) {
  LogManager log{std::filesystem::path(walPath)};
  TransactionManager transactions(log);
  const auto transaction = transactions.begin();
  FileManager files;
  files.openDatabase(std::filesystem::path(databasePath));
  auto before = files.readPage(kFirstUserPageId);
  auto after = before;
  after.payload()[0] = 0x42;
  const auto lsn = log.appendWithLsn(LogRecordType::PageImageWritten, [&](LogSequenceNumber assigned) {
    after.setPageLsn(assigned.value());
    return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
      .transactionId = transaction.id(), .pageId = kFirstUserPageId,
      .previousTxLsn = 0, .pageFormatVersion = Page::kFormatVersionV2,
      .beforeImage = before.serialize(), .afterImage = after.serialize(),
    });
  });
  after.setPageLsn(lsn.value());
  files.writePage(after);
  files.flush();
  if (commit) transactions.commit(transaction.id());
  std::ofstream ready(readyPath, std::ios::binary | std::ios::trunc);
  ready << (commit ? "commit" : "incomplete");
  ready.flush();
  if (!ready) return 2;
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}


int runCrashRecoveryChild(const char* databasePath, const char* walPath, const char* readyPath) {
  class PausingApplier final : public RedoApplier, public UndoApplier {
  public:
    PausingApplier(PageImageRecoveryApplier& delegate, const char* readyPath)
      : delegate_(delegate), readyPath_(readyPath) {}
    void applyRedo(const RecoveryAction& action) override {
      delegate_.applyRedo(action);
      pause();
    }
    void applyUndo(const RecoveryAction& action) override {
      delegate_.applyUndo(action);
      pause();
    }
  private:
    void pause() {
      std::ofstream ready(readyPath_, std::ios::binary | std::ios::trunc);
      ready << "applier-returned";
      ready.flush();
      if (!ready) throw std::runtime_error("Failed to signal recovery crash barrier");
      for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    PageImageRecoveryApplier& delegate_;
    const char* readyPath_;
  };

  LogManager log{std::filesystem::path(walPath)};
  FileManager files;
  files.openDatabase(std::filesystem::path(databasePath));
  RecoveryEngine engine;
  const auto plan = engine.analyze(log.prepareRecovery());
  PageImageRecoveryApplier pageApplier(files);
  PausingApplier pausing(pageApplier, readyPath);
  UndoRedoRecoveryExecutor executor(pausing, pausing);
  (void)executor.execute(plan);
  return 0;
}

int runCrashMetadataChild(const char* databasePath, std::size_t failWriteAt, bool extendDirectory) {
  if (extendDirectory) {
    DatabaseRuntime runtime(databasePath);
    IoFaultInjector::arm(IoFaultPoint::PageWrite, failWriteAt);
    try { runtime.records().put("stable", "spill", std::vector<Byte>{42}); }
    catch (const std::system_error& error) {
      if (error.code() == std::errc::io_error) std::_Exit(91);
      throw;
    }
    return 2;
  }
  FileManager files;
  files.openDatabase(databasePath);
  LogManager wal(databaseWalPath(databasePath));
  TransactionManager transactions(wal);
  StorageMutationCoordinator coordinator(files, wal, transactions);
  FreePageManager freePages(files, coordinator);
  freePages.load();
  MetadataManager metadata(files, freePages, coordinator);
  metadata.loadMetadata();
  IoFaultInjector::arm(IoFaultPoint::PageWrite, failWriteAt);
  try { (void)metadata.createTable("crash_tx"); }
  catch (...) { std::_Exit(91); }
  return 2;
}

void test_crash_recovery(const char* executablePath) {
  const auto walPath = uniqueTempPath(".wal");
  const auto readyPath = uniqueTempPath(".ready");
#ifdef _WIN32
  std::wstring command = quote(std::filesystem::path(executablePath).wstring()) +
    L" --crash-commit-child " + quote(walPath.wstring()) + L" " + quote(readyPath.wstring());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process))
    throw std::runtime_error("Failed to start crash-test child process");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(readyPath) && std::chrono::steady_clock::now() < deadline) Sleep(10);
  if (!std::filesystem::exists(readyPath)) {
    TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    throw std::runtime_error("Crash-test child missed durable-commit barrier");
  }
  if (!TerminateProcess(process.hProcess, 99)) throw std::runtime_error("Failed to terminate crash-test child");
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
  const auto child = fork();
  if (child == -1) throw std::runtime_error("Failed to fork crash-test child");
  if (child == 0) {
    execl(executablePath, executablePath, "--crash-commit-child", walPath.c_str(), readyPath.c_str(), nullptr);
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(readyPath) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!std::filesystem::exists(readyPath)) {
    kill(child, SIGKILL); waitpid(child, nullptr, 0);
    throw std::runtime_error("Crash-test child missed durable-commit barrier");
  }
  if (kill(child, SIGKILL) != 0) throw std::runtime_error("Failed to kill crash-test child");
  waitpid(child, nullptr, 0);
#endif
  {
    LogManager reopened{walPath};
    const auto records = reopened.replay();
    assert(records.size() == 3);
    assert(records[0].type() == LogRecordType::TransactionStarted);
    assert(records[1].type() == LogRecordType::TransactionCommitted);
    assert(records[2].type() == LogRecordType::Checkpoint);
    const auto committedId = LogPayloadCodec::decodeTransaction(records[1].payload()).transactionId;
    TransactionManager transactions(reopened);
    assert(transactions.begin().id().value() > committedId.value());
  }
  std::filesystem::remove(walPath);
  std::filesystem::remove(readyPath);

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto crashMetadata = [&](const std::filesystem::path& databasePath, std::size_t failWriteAt, bool extendDirectory) {
#ifdef _WIN32
    std::wstring command = quote(std::filesystem::path(executablePath).wstring()) +
      L" --crash-metadata-child " + quote(databasePath.wstring()) + L" " + std::to_wstring(failWriteAt) +
      (extendDirectory ? L" extend" : L" create");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
      throw std::runtime_error("Failed to start metadata crash child");
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, INFINITE);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
      throw std::runtime_error("Metadata crash child timed out");
    }
    DWORD exitCode{};
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (exitCode != 91) throw std::runtime_error("Metadata crash child missed injected page-write failure");
#else
    const auto child = fork();
    if (child == -1) throw std::runtime_error("Failed to fork metadata crash child");
    if (child == 0) {
      const auto occurrence = std::to_string(failWriteAt);
      execl(executablePath, executablePath, "--crash-metadata-child", databasePath.c_str(), occurrence.c_str(),
            extendDirectory ? "extend" : "create", nullptr);
      _exit(127);
    }
    int childStatus{};
    if (waitpid(child, &childStatus, 0) != child || !WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 91) {
      throw std::runtime_error("Metadata crash child missed injected page-write failure");
    }
#endif
  };
  for (std::size_t failWriteAt = 1; failWriteAt <= 4; ++failWriteAt) {
    const auto databasePath = uniqueTempPath("_metadata_crash.zdb");
    {
      FileManager files;
      files.createDatabase(databasePath);
      LogManager wal(databaseWalPath(databasePath));
      TransactionManager transactions(wal);
      StorageMutationCoordinator coordinator(files, wal, transactions);
      FreePageManager freePages(files, coordinator);
      freePages.load();
      MetadataManager metadata(files, freePages, coordinator);
      (void)metadata.createTable("stable");
      files.closeDatabase();
    }
    crashMetadata(databasePath, failWriteAt, false);
    {
      FileManager reopened;
      reopened.openDatabase(databasePath);
      LogManager wal(databaseWalPath(databasePath));
      TransactionManager transactions(wal);
      StorageMutationCoordinator coordinator(reopened, wal, transactions);
      FreePageManager freePages(reopened, coordinator);
      freePages.load();
      MetadataManager metadata(reopened, freePages, coordinator);
      metadata.loadMetadata();
      assert(metadata.tableExists("stable"));
      assert(!metadata.tableExists("crash_tx"));
      (void)StorageConsistencyChecker{}.check(reopened, freePages, metadata, wal);
    }
    removeTestDatabaseArtifacts(databasePath);
  }
  const auto directoryPath = uniqueTempPath("_directory_crash.zdb");
  const std::vector<Byte> stableValue{1, 2, 3};
  constexpr auto stableRecords = PageDirectory::kSinglePageCapacity - 1;
  {
    DatabaseRuntime runtime(directoryPath, true);
    (void)runtime.createTable("stable");
    for (std::size_t i = 0; i < stableRecords; ++i) {
      runtime.records().put("stable", std::to_string(i), stableValue);
    }
  }
  for (const std::size_t failWriteAt : {4, 5}) {
    crashMetadata(directoryPath, failWriteAt, true);
    DatabaseRuntime runtime(directoryPath);
    assert(!runtime.records().get("stable", "spill"));
    assert(runtime.records().get("stable", std::to_string(stableRecords - 1)) == stableValue);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + stableRecords + 1);
  }
  {
    DatabaseRuntime runtime(directoryPath);
    runtime.records().put("stable", "spill", stableValue);
  }
  {
    DatabaseRuntime runtime(directoryPath);
    assert(runtime.records().get("stable", "spill") == stableValue);
    assert(runtime.checkConsistency().pagesChecked == kFirstUserPageId + stableRecords + 3);
  }
  removeTestDatabaseArtifacts(directoryPath);
#endif

  {
    const auto databasePath = uniqueTempPath("_bad_page.zdb");
    const auto walPath = uniqueTempPath("_bad_page.wal");
    {
      LogManager log{walPath};
      (void)log.appendWithLsn(LogRecordType::DatabaseCreated, [](LogSequenceNumber) { return std::vector<Byte>{}; });
      FileManager files;
      files.createDatabase(databasePath);
      Page page;
      page.header().version = Page::kFormatVersionV2;
      page.header().pageId = kFirstUserPageId;
      page.header().pageType = PageType::Table;
      page.header().usedBytes = 1;
      page.payload()[0] = 0x11;
      page.setPageLsn(999);
      files.writePage(page);
      files.flush();
      bool futureLsnRejected = false;
      try { (void)RecoveryCoordinator{}.recover(log, files); }
      catch (const std::runtime_error&) { futureLsnRejected = true; }
      assert(futureLsnRejected);
      page.setPageLsn(0);
      files.writePage(page);
      files.flush();
      files.closeDatabase();
      {
        std::fstream corrupt(databasePath, std::ios::binary | std::ios::in | std::ios::out);
        const auto offset = static_cast<std::streamoff>(DatabaseHeader::serializedSize()) +
          static_cast<std::streamoff>(kFirstUserPageId) * static_cast<std::streamoff>(kDefaultPageSize) +
          static_cast<std::streamoff>(kDefaultPageSize - 1);
        corrupt.seekg(offset);
        char byte{};
        corrupt.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        corrupt.seekp(offset);
        corrupt.write(&byte, 1);
        corrupt.flush();
      }
      bool pageCorruptionRejected = false;
      try {
        FileManager damaged;
        damaged.openDatabase(databasePath);
      }
      catch (const std::exception&) { pageCorruptionRejected = true; }
      assert(pageCorruptionRejected);
    }
    std::filesystem::remove(databasePath);
    std::filesystem::remove(walPath);
  }

  {
    const auto databasePath = uniqueTempPath("_missing_wal.zdb");
    const auto missingWalPath = databaseWalPath(databasePath);
    {
      FileManager files;
      files.createDatabase(databasePath);
      Page page;
      page.header().pageId = kFirstUserPageId;
      page.header().pageType = PageType::Table;
      page.header().usedBytes = 1;
      files.writePage(page);
      files.flush();
    }
    std::filesystem::remove(missingWalPath);
    bool rejected = false;
    try {
      FileManager files;
      files.openDatabase(databasePath);
    } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    std::filesystem::remove(databasePath);
    std::filesystem::remove(missingWalPath);
  }

  for (const bool commit : {false, true}) {
    const auto databasePath = uniqueTempPath(".zdb");
    const auto pageWalPath = databaseWalPath(databasePath);
    const auto pageReadyPath = uniqueTempPath(".ready");
    {
      FileManager files;
      files.createDatabase(databasePath);
      Page original;
      original.header().version = Page::kFormatVersionV2;
      original.header().pageId = kFirstUserPageId;
      original.header().pageType = PageType::Table;
      original.header().usedBytes = 1;
      original.payload()[0] = 0x11;
      files.writePage(original);
      files.flush();
    }
#ifdef _WIN32
    std::wstring command = quote(std::filesystem::path(executablePath).wstring()) +
      L" --crash-page-child " + quote(databasePath.wstring()) + L" " + quote(pageWalPath.wstring()) +
      L" " + quote(pageReadyPath.wstring()) + (commit ? L" commit" : L" abort");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
      throw std::runtime_error("Failed to start page crash-test child");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(pageReadyPath) && std::chrono::steady_clock::now() < deadline) Sleep(10);
    if (!std::filesystem::exists(pageReadyPath)) {
      TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, INFINITE);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
      throw std::runtime_error("Page crash-test child missed barrier");
    }
    TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
    const auto child = fork();
    if (child == -1) throw std::runtime_error("Failed to fork page crash-test child");
    if (child == 0) {
      execl(executablePath, executablePath, "--crash-page-child", databasePath.c_str(), pageWalPath.c_str(),
            pageReadyPath.c_str(), commit ? "commit" : "abort", nullptr);
      _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(pageReadyPath) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!std::filesystem::exists(pageReadyPath)) {
      kill(child, SIGKILL); waitpid(child, nullptr, 0);
      throw std::runtime_error("Page crash-test child missed barrier");
    }
    kill(child, SIGKILL); waitpid(child, nullptr, 0);
#endif
    const auto recoveryReadyPath = uniqueTempPath("_recovery.ready");
#ifdef _WIN32
    std::wstring recoveryCommand = quote(std::filesystem::path(executablePath).wstring()) +
      L" --crash-recovery-child " + quote(databasePath.wstring()) + L" " + quote(pageWalPath.wstring()) +
      L" " + quote(recoveryReadyPath.wstring());
    STARTUPINFOW recoveryStartup{}; recoveryStartup.cb = sizeof(recoveryStartup);
    PROCESS_INFORMATION recoveryProcess{};
    if (!CreateProcessW(nullptr, recoveryCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &recoveryStartup, &recoveryProcess))
      throw std::runtime_error("Failed to start recovery crash-test child");
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(recoveryReadyPath) && std::chrono::steady_clock::now() < recoveryDeadline) Sleep(10);
    if (!std::filesystem::exists(recoveryReadyPath)) {
      TerminateProcess(recoveryProcess.hProcess, 3); WaitForSingleObject(recoveryProcess.hProcess, INFINITE);
      CloseHandle(recoveryProcess.hThread); CloseHandle(recoveryProcess.hProcess);
      throw std::runtime_error("Recovery child missed applier barrier");
    }
    TerminateProcess(recoveryProcess.hProcess, 99); WaitForSingleObject(recoveryProcess.hProcess, INFINITE);
    CloseHandle(recoveryProcess.hThread); CloseHandle(recoveryProcess.hProcess);
#else
    const auto recoveryChild = fork();
    if (recoveryChild == -1) throw std::runtime_error("Failed to fork recovery crash-test child");
    if (recoveryChild == 0) {
      execl(executablePath, executablePath, "--crash-recovery-child", databasePath.c_str(), pageWalPath.c_str(),
            recoveryReadyPath.c_str(), nullptr);
      _exit(127);
    }
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(recoveryReadyPath) && std::chrono::steady_clock::now() < recoveryDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!std::filesystem::exists(recoveryReadyPath)) {
      kill(recoveryChild, SIGKILL); waitpid(recoveryChild, nullptr, 0);
      throw std::runtime_error("Recovery child missed applier barrier");
    }
    kill(recoveryChild, SIGKILL); waitpid(recoveryChild, nullptr, 0);
#endif
    std::filesystem::remove(recoveryReadyPath);
    {
      LogManager log{pageWalPath};
      FileManager files;
      files.openDatabase(databasePath);
      RecoveryCoordinator coordinator;
      const auto firstRecovery = coordinator.recover(log, files);
      assert(firstRecovery.execution.redoApplied + firstRecovery.execution.undoApplied == 1);
      assert(files.readPage(kFirstUserPageId).payload()[0] == (commit ? 0x42 : 0x11));
      const auto secondRecovery = coordinator.recover(log, files);
      assert(secondRecovery.execution.redoApplied + secondRecovery.execution.undoApplied == 1);
      assert(files.readPage(kFirstUserPageId).payload()[0] == (commit ? 0x42 : 0x11));
    }
    std::filesystem::remove(databasePath);
    std::filesystem::remove(pageWalPath);
    std::filesystem::remove(pageReadyPath);
    std::filesystem::remove(recoveryReadyPath);
  }
}
