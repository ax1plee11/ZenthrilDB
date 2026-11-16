#include "core/DatabaseRuntime.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
namespace {
constexpr int kInjectedCrashCode = 91;
constexpr auto kChildTimeout = std::chrono::seconds(30);

enum class FrameState { Absent, Partial, Buffered, Durable };
struct CrashPhase {
  const char* name;
  std::optional<IoFaultPoint> fault;
  FrameState frame;
};

constexpr CrashPhase kPhases[] = {
  {"before", std::nullopt, FrameState::Absent},
  {"preparing", IoFaultPoint::CheckpointPreparing, FrameState::Absent},
  {"wal-durable", IoFaultPoint::CheckpointWalDurable, FrameState::Absent},
  {"pages-durable", IoFaultPoint::CheckpointPagesDurable, FrameState::Absent},
  {"partial-frame", IoFaultPoint::CheckpointPartialFrame, FrameState::Partial},
  {"buffered-frame", IoFaultPoint::CheckpointFrameBuffered, FrameState::Buffered},
  {"durable-frame", IoFaultPoint::CheckpointDurable, FrameState::Durable},
  {"after", std::nullopt, FrameState::Durable},
};

std::string readFixture(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Failed to read checkpoint crash fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void runCrashProcess(const char* executablePath, const std::filesystem::path& database, const char* phase) {
#ifdef _WIN32
  (void)executablePath;
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size()) {
    throw std::runtime_error("Failed to determine checkpoint test executable path");
  }
  executable.resize(length);
  const auto workingDirectory = database.parent_path();
  const std::string_view narrowPhase(phase);
  const std::wstring widePhase(narrowPhase.begin(), narrowPhase.end());
  auto command = L"\"" + executable + L"\" --checkpoint-crash-child \"" +
    database.filename().wstring() + L"\" " + widePhase;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, workingDirectory.c_str(), &startup, &process)) {
    throw std::runtime_error("Failed to start checkpoint crash child");
  }
  CloseHandle(process.hThread);
  const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kChildTimeout);
  if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 3);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Checkpoint crash child did not exit");
  }
  DWORD exitCode{};
  const auto receivedStatus = GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  if (!receivedStatus || exitCode != kInjectedCrashCode) {
    throw std::runtime_error(std::string("Checkpoint child missed crash phase ") + phase);
  }
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Failed to fork checkpoint crash child");
  if (child == 0) {
    execl(executablePath, executablePath, "--checkpoint-crash-child", database.c_str(), phase, nullptr);
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
  int status{};
  for (;;) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) throw std::runtime_error("Failed to wait for checkpoint crash child");
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Checkpoint crash child did not exit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != kInjectedCrashCode) {
    throw std::runtime_error(std::string("Checkpoint child missed crash phase ") + phase);
  }
#endif
}
} // namespace
#endif

int runCheckpointCrashChild(const char* path, const char* name) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  DatabaseRuntime runtime{std::filesystem::path(path)};
  for (const auto& phase : kPhases) {
    if (std::string_view(name) != phase.name) continue;
    if (std::string_view(name) == "before") std::_Exit(kInjectedCrashCode);
    if (phase.fault) IoFaultInjector::armCrash(*phase.fault);
    (void)runtime.checkpoint();
    if (std::string_view(name) == "after") std::_Exit(kInjectedCrashCode);
    return 2;
  }
#else
  (void)path;
  (void)name;
#endif
  return 2;
}

void test_checkpoint_crash(const char* executablePath) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
    ("zenthrildb_checkpoint_crash_" + std::to_string(stamp));
  if (!std::filesystem::create_directory(directory)) {
    throw std::runtime_error("Failed to create checkpoint crash fixture directory");
  }
  const std::vector<Byte> value{9, 2, 4};
  for (const auto& phase : kPhases) {
    const auto database = directory / (std::string(phase.name) + ".zdb");
    const auto wal = databaseWalPath(database);
    std::uint64_t walRecordsBefore = 0;
    {
      DatabaseRuntime runtime(database, true);
      (void)runtime.createTable("records");
      runtime.records().put("records", "confirmed", value);
      walRecordsBefore = runtime.checkConsistency().walRecordsChecked;
    }
    const auto walBefore = readFixture(wal);
    const auto databaseBefore = readFixture(database);
    runCrashProcess(executablePath, database, phase.name);
    const auto walAfterCrash = readFixture(wal);
    assert(walAfterCrash.starts_with(walBefore));
    assert(readFixture(database) == databaseBefore);
    if (phase.frame == FrameState::Absent) {
      assert(walAfterCrash == walBefore);
    } else if (phase.frame == FrameState::Partial || phase.frame == FrameState::Durable) {
      assert(walAfterCrash.size() > walBefore.size());
    }

    std::string walAfterRecovery;
    for (int open = 0; open < 2; ++open) {
      DatabaseRuntime recovered(database);
      assert(recovered.records().get("records", "confirmed") == value);
      const auto recoveredRecords = recovered.checkConsistency().walRecordsChecked;
      const auto currentWal = readFixture(wal);
      if (phase.frame == FrameState::Durable ||
          (phase.frame == FrameState::Buffered && currentWal.size() > walBefore.size())) {
        assert(currentWal == walAfterCrash);
        assert(recoveredRecords == walRecordsBefore + 1);
      } else {
        assert(currentWal == walBefore);
        assert(recoveredRecords == walRecordsBefore);
      }
      if (open == 0) walAfterRecovery = currentWal;
      else assert(currentWal == walAfterRecovery);
    }
    assert(readFixture(database) == databaseBefore);
    removeTestDatabaseArtifacts(database);
  }
  std::filesystem::remove(directory);
#else
  (void)executablePath;
#endif
}
