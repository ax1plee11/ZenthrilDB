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
#include <thread>

#if defined(ZENTHRILDB_ENABLE_FAULT_INJECTION) && defined(_WIN32)
#include <Windows.h>
#elif defined(ZENTHRILDB_ENABLE_FAULT_INJECTION)
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace zenthrildb;

#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
namespace {

constexpr int kCrashExitCode = 91;
constexpr auto kCrashTimeout = std::chrono::seconds(30);

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read snapshot crash fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void runCrashChild(const char* executablePath, const std::filesystem::path& source,
                   const std::filesystem::path& destination) {
#ifdef _WIN32
  const auto executable = std::filesystem::absolute(executablePath).wstring();
  auto command = L"\"" + executable + L"\" --snapshot-crash-child \"" + source.wstring() +
    L"\" \"" + destination.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    throw std::runtime_error("Failed to start snapshot crash child");
  }
  CloseHandle(process.hThread);
  const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kCrashTimeout);
  const auto waitResult = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count()));
  if (waitResult != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 3);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Snapshot crash child did not exit");
  }
  DWORD exitCode{};
  const auto receivedStatus = GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  if (!receivedStatus || exitCode != kCrashExitCode) {
    throw std::runtime_error("Snapshot child missed the crash publication barrier");
  }
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Failed to fork snapshot crash child");
  if (child == 0) {
    execl(executablePath, executablePath, "--snapshot-crash-child", source.c_str(), destination.c_str(), nullptr);
    _exit(127);
  }
  int status{};
  const auto deadline = std::chrono::steady_clock::now() + kCrashTimeout;
  for (;;) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) throw std::runtime_error("Failed to wait for snapshot crash child");
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Snapshot crash child did not exit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != kCrashExitCode) {
    throw std::runtime_error("Snapshot child missed the crash publication barrier");
  }
#endif
}

} // namespace
#endif

int runSnapshotCrashChild(const char* sourcePath, const char* destPath) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  IoFaultInjector::armCrash(IoFaultPoint::SnapshotPublish);
  DatabaseRuntime::snapshotTo(sourcePath, destPath);
#else
  (void)sourcePath;
  (void)destPath;
#endif
  return 2;
}

void test_snapshot_crash(const char* executablePath) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_snapshot_crash_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto source = directory / "source.zdb";
  const auto interrupted = directory / "interrupted";
  const auto retry = directory / "retry";
  const std::vector<Byte> value{0, 42, 255};
  {
    DatabaseRuntime runtime(source, true);
    (void)runtime.createTable("records");
    runtime.records().put("records", "durable", value);
  }
  const auto sourceData = readBytes(source);
  const auto sourceWal = readBytes(databaseWalPath(source));
  runCrashChild(executablePath, source, interrupted);

  assert(!std::filesystem::exists(interrupted));
  assert(readBytes(source) == sourceData);
  assert(readBytes(databaseWalPath(source)) == sourceWal);

  std::filesystem::path staging;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_directory() && entry.path().filename().string().starts_with("interrupted.snapshot-")) {
      assert(staging.empty());
      staging = entry.path();
    }
  }
  assert(!staging.empty());
  const auto unfinished = staging / source.filename();
  assert(std::filesystem::is_regular_file(unfinished));
  assert(std::filesystem::is_regular_file(databaseWalPath(unfinished)));

  DatabaseRuntime::snapshotTo(source, retry);
  const auto published = retry / source.filename();
  for (int open = 0; open < 2; ++open) {
    DatabaseRuntime runtime(published);
    assert(runtime.records().get("records", "durable") == value);
  }
  assert(readBytes(source) == sourceData);
  assert(readBytes(databaseWalPath(source)) == sourceWal);

  removeTestDatabaseArtifacts(published);
  std::filesystem::remove(retry);
  removeTestDatabaseArtifacts(unfinished);
  std::filesystem::remove(staging);
  removeTestDatabaseArtifacts(source);
  std::filesystem::remove(directory);
#else
  (void)executablePath;
#endif
}
