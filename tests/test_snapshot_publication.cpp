#include "core/DatabaseFileLock.hpp"
#include "core/DatabaseRuntime.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
#include <cstdlib>
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

namespace {

const std::vector<Byte> kValue{0, 31, 255};

std::string readFixture(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read snapshot publication fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void publishFixture(const std::filesystem::path& source, const std::filesystem::path& destination,
                    bool requireBaseline, bool reduceHistory) {
  if (reduceHistory) DatabaseRuntime::reduceWalTo(source, destination);
  else DatabaseRuntime::snapshotTo(source, destination, requireBaseline);
}

void assertPublished(const std::filesystem::path& database, bool requireBaseline, bool reduceHistory) {
  const auto wal = databaseWalPath(database);
  const auto dataBeforeVerification = readFixture(database);
  const auto walBeforeVerification = readFixture(wal);
  const auto manifest = DatabaseRuntime::verifySnapshot(database);
  assert(manifest.identity.baselineRequired == requireBaseline);
  assert(manifest.identity.reducedHistory == reduceHistory);
  assert(readFixture(database) == dataBeforeVerification);
  assert(readFixture(wal) == walBeforeVerification);
  for (int attempt = 0; attempt < 2; ++attempt) {
    DatabaseRuntime reopened(database);
    assert(reopened.records().get("records", "confirmed") == kValue);
    (void)reopened.checkConsistency();
  }
}

void clearFixtureDirectory(const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (std::filesystem::is_directory(entry.symlink_status())) {
      throw std::runtime_error("Unexpected nested directory in snapshot publication fixture");
    }
    std::filesystem::remove(entry.path());
  }
  std::filesystem::remove(directory);
}

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
constexpr int kCrashExitCode = 91;
constexpr auto kCrashTimeout = std::chrono::seconds(45);

struct PublicationPhase {
  const char* name;
  IoFaultPoint point;
  bool published;
};

constexpr PublicationPhase kPhases[] = {
  {"directory", IoFaultPoint::SnapshotDirectory, false},
  {"data-partial", IoFaultPoint::SnapshotDataPartial, false},
  {"data-copied", IoFaultPoint::SnapshotDataCopied, false},
  {"wal-partial", IoFaultPoint::SnapshotWalPartial, false},
  {"wal-copied", IoFaultPoint::SnapshotWalCopied, false},
  {"wal-durable", IoFaultPoint::SnapshotWalDurable, false},
  {"data-durable", IoFaultPoint::SnapshotDataDurable, false},
  {"manifest-written", IoFaultPoint::SnapshotManifestWritten, false},
  {"manifest-durable", IoFaultPoint::SnapshotManifestDurable, false},
  {"validating", IoFaultPoint::SnapshotValidating, false},
  {"publish", IoFaultPoint::SnapshotPublish, false},
  {"renamed", IoFaultPoint::SnapshotRenamed, true},
  {"parent-durable", IoFaultPoint::SnapshotParentDurable, true},
};

constexpr PublicationPhase kIoFailures[] = {
  {"create-directory", IoFaultPoint::SnapshotCreateDirectory, false},
  {"open-file", IoFaultPoint::SnapshotOpenFile, false},
  {"read-file", IoFaultPoint::SnapshotReadFile, false},
  {"write-file", IoFaultPoint::SnapshotWriteFile, false},
  {"close-file", IoFaultPoint::SnapshotCloseFile, false},
  {"sync-file", IoFaultPoint::SnapshotSyncFile, false},
  {"sync-directory", IoFaultPoint::SnapshotSyncDirectory, false},
  {"rename", IoFaultPoint::SnapshotRename, false},
  {"sync-parent", IoFaultPoint::SnapshotSyncParent, true},
  {"no-space", IoFaultPoint::NoSpace, false},
};

constexpr PublicationPhase kReductionPhases[] = {
  {"reduction-boundary", IoFaultPoint::ReductionBoundary, false},
  {"reduction-partial", IoFaultPoint::ReductionPartialFrame, false},
  {"reduction-written", IoFaultPoint::ReductionAnchorWritten, false},
};

void runCrashProcess(const char* executablePath, const std::filesystem::path& source,
                     const std::filesystem::path& destination, const char* phase,
                     bool requireBaseline, bool reduceHistory) {
  const auto prefix = reduceHistory ? "reduced/" : (requireBaseline ? "required/" : "");
  const auto requestedPhase = std::string(prefix) + phase;
#ifdef _WIN32
  (void)executablePath;
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size()) {
    throw std::runtime_error("Failed to determine snapshot publication test executable path");
  }
  executable.resize(length);
  const auto workingDirectory = source.parent_path();
  const std::string_view narrowPhase(requestedPhase);
  const std::wstring widePhase(narrowPhase.begin(), narrowPhase.end());
  auto command = L"\"" + executable + L"\" --snapshot-publication-child \"" +
    source.filename().wstring() + L"\" \"" + destination.filename().wstring() + L"\" " + widePhase;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, workingDirectory.c_str(), &startup, &process)) {
    throw std::runtime_error("Failed to start snapshot publication crash child");
  }
  CloseHandle(process.hThread);
  const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kCrashTimeout);
  if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 3);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Snapshot publication crash child did not exit");
  }
  DWORD exitCode{};
  const auto receivedStatus = GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  if (!receivedStatus || exitCode != kCrashExitCode) {
    throw std::runtime_error(std::string("Snapshot child missed publication crash phase ") + phase);
  }
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Failed to fork snapshot publication crash child");
  if (child == 0) {
    execl(executablePath, executablePath, "--snapshot-publication-child",
          source.c_str(), destination.c_str(), requestedPhase.c_str(), nullptr);
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + kCrashTimeout;
  int status{};
  for (;;) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) throw std::runtime_error("Failed to wait for snapshot publication child");
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Snapshot publication crash child did not exit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != kCrashExitCode) {
    throw std::runtime_error(std::string("Snapshot child missed publication crash phase ") + phase);
  }
#endif
}

void checkIncompleteStaging(const std::filesystem::path& destination, const std::filesystem::path& source,
                            IoFaultPoint point) {
  const auto prefix = destination.filename().string() + ".snapshot-";
  std::filesystem::path staging;
  for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path())) {
    if (entry.path().filename().string().starts_with(prefix)) {
      assert(staging.empty());
      assert(std::filesystem::is_directory(entry.symlink_status()));
      staging = entry.path();
    }
  }
  assert(!staging.empty());
  const auto stagedData = staging / source.filename();
  if (point == IoFaultPoint::SnapshotDataPartial || point == IoFaultPoint::SnapshotWalPartial) {
    const auto originalFile = point == IoFaultPoint::SnapshotDataPartial ? source : databaseWalPath(source);
    const auto partialFile = point == IoFaultPoint::SnapshotDataPartial ? stagedData : databaseWalPath(stagedData);
    const auto partialSize = std::filesystem::file_size(partialFile);
    assert(partialSize > 0 && partialSize < std::filesystem::file_size(originalFile));
  }
  if (point == IoFaultPoint::ReductionPartialFrame) {
    const auto size = std::filesystem::file_size(staging / "identity-wal.partial");
    assert(size > WalIdentity::kSize && size < WalIdentity::kSize + LogRecord::kHeaderSize + 52);
  }
  clearFixtureDirectory(staging);
}

void assertNoStaging(const std::filesystem::path& destination) {
  const auto prefix = destination.filename().string() + ".snapshot-";
  for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path())) {
    assert(!entry.path().filename().string().starts_with(prefix));
  }
}

void checkIoFailure(const PublicationPhase& phase, const std::filesystem::path& source,
                    const std::filesystem::path& destination, bool requireBaseline, bool reduceHistory) {
  IoFaultInjector::arm(phase.point);
  bool rejected = false;
  try {
    publishFixture(source, destination, requireBaseline, reduceHistory);
  } catch (const std::system_error& error) {
    const auto expectedError = phase.point == IoFaultPoint::NoSpace ? std::errc::no_space_on_device : std::errc::io_error;
    rejected = error.code() == expectedError;
  } catch (...) {
    IoFaultInjector::clear();
    throw;
  }
  IoFaultInjector::clear();
  if (!rejected) throw std::runtime_error(std::string("Snapshot missed injected I/O failure at ") + phase.name);
  assertNoStaging(destination);
}
#endif
} // namespace

int runSnapshotPublicationChild(const char* source, const char* destination, const char* phaseName) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  std::string_view requested(phaseName);
  const bool reduceHistory = requested.starts_with("reduced/");
  const bool requireBaseline = reduceHistory || requested.starts_with("required/");
  if (requireBaseline) requested.remove_prefix(reduceHistory ? 8 : 9);
  if (requested == "before-call") std::_Exit(kCrashExitCode);
  if (requested == "after-return") {
    publishFixture(source, destination, requireBaseline, reduceHistory);
    {
      DatabaseFileLock sourceLock(source);
    }
    std::_Exit(kCrashExitCode);
  }
  for (const auto& phase : kPhases) {
    if (requested != phase.name) continue;
    IoFaultInjector::armCrash(phase.point);
    publishFixture(source, destination, requireBaseline, reduceHistory);
    return 2;
  }
  if (reduceHistory) {
    for (const auto& phase : kReductionPhases) {
      if (requested != phase.name) continue;
      IoFaultInjector::armCrash(phase.point);
      publishFixture(source, destination, true, true);
      return 2;
    }
  }
#else
  (void)source;
  (void)destination;
  (void)phaseName;
#endif
  return 2;
}

void test_snapshot_publication(const char* executablePath, bool requireBaseline, bool reduceHistory) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto parent = std::filesystem::temp_directory_path() /
    ("zenthrildb_snapshot_publication_" + std::to_string(stamp));
  if (!std::filesystem::create_directory(parent)) {
    throw std::runtime_error("Failed to create snapshot publication fixture directory");
  }
  const auto source = parent / "source.zdb";
  const auto sourceWal = databaseWalPath(source);
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "confirmed", kValue);
  }
  const auto originalData = readFixture(source);
  const auto originalWal = readFixture(sourceWal);
  const auto assertSourceUnchanged = [&] {
    assert(readFixture(source) == originalData);
    assert(readFixture(sourceWal) == originalWal);
  };

  const auto existing = parent / "existing";
  if (!std::filesystem::create_directory(existing)) {
    throw std::runtime_error("Failed to create existing destination fixture");
  }
  bool rejected = false;
  try {
    publishFixture(source, existing, requireBaseline, reduceHistory);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected && std::filesystem::is_empty(existing));
  std::filesystem::remove(existing);
  assertSourceUnchanged();

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto beforeCall = parent / "crash-before-call";
  runCrashProcess(executablePath, source, beforeCall, "before-call", requireBaseline, reduceHistory);
  assert(!std::filesystem::exists(beforeCall));
  assertNoStaging(beforeCall);
  assertSourceUnchanged();

  const auto afterReturn = parent / "crash-after-return";
  runCrashProcess(executablePath, source, afterReturn, "after-return", requireBaseline, reduceHistory);
  assertPublished(afterReturn / source.filename(), requireBaseline, reduceHistory);
  assertNoStaging(afterReturn);
  assertSourceUnchanged();
  clearFixtureDirectory(afterReturn);

  const auto verifyIo = [&](const PublicationPhase& phase) {
    const auto failed = parent / (std::string("io-") + phase.name);
    checkIoFailure(phase, source, failed, requireBaseline, reduceHistory);
    assertSourceUnchanged();
    if (phase.published) {
      assertPublished(failed / source.filename(), requireBaseline, reduceHistory);
      clearFixtureDirectory(failed);
    } else {
      assert(!std::filesystem::exists(failed));
      publishFixture(source, failed, requireBaseline, reduceHistory);
      assertPublished(failed / source.filename(), requireBaseline, reduceHistory);
      clearFixtureDirectory(failed);
    }
    assertSourceUnchanged();
  };
  const auto verifyCrashAndIo = [&](const PublicationPhase& phase) {
    const auto crashed = parent / (std::string("crash-") + phase.name);
    runCrashProcess(executablePath, source, crashed, phase.name, requireBaseline, reduceHistory);
    assertSourceUnchanged();
    if (phase.published) {
      assertPublished(crashed / source.filename(), requireBaseline, reduceHistory);
      clearFixtureDirectory(crashed);
    } else {
      assert(!std::filesystem::exists(crashed));
      checkIncompleteStaging(crashed, source, phase.point);
      publishFixture(source, crashed, requireBaseline, reduceHistory);
      assertPublished(crashed / source.filename(), requireBaseline, reduceHistory);
      clearFixtureDirectory(crashed);
    }
    assertSourceUnchanged();

    verifyIo(phase);
  };
  for (const auto& phase : kPhases) verifyCrashAndIo(phase);
  if (reduceHistory) for (const auto& phase : kReductionPhases) verifyCrashAndIo(phase);
  for (const auto& phase : kIoFailures) verifyIo(phase);
#else
  (void)executablePath;
#endif

  const auto finished = parent / "finished";
  publishFixture(source, finished, requireBaseline, reduceHistory);
  assertPublished(finished / source.filename(), requireBaseline, reduceHistory);
  assertSourceUnchanged();
  const auto publishedData = finished / source.filename();
  const auto publishedWal = databaseWalPath(publishedData);
  const auto dataBeforeRepeat = readFixture(publishedData);
  const auto walBeforeRepeat = readFixture(publishedWal);
  bool repeatRejected = false;
  try {
    publishFixture(source, finished, requireBaseline, reduceHistory);
  } catch (const std::invalid_argument&) {
    repeatRejected = true;
  }
  assert(repeatRejected);
  assert(readFixture(publishedData) == dataBeforeRepeat);
  assert(readFixture(publishedWal) == walBeforeRepeat);
  (void)DatabaseRuntime::verifySnapshot(publishedData);
  assertSourceUnchanged();
  clearFixtureDirectory(finished);
  removeTestDatabaseArtifacts(source);
  std::filesystem::remove(parent);
}
