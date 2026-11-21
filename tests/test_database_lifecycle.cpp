#include "core/DatabaseRuntime.hpp"
#include "core/BufferManager.hpp"
#include "recovery/RecoveryCoordinator.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iterator>
#include <iostream>
#include <latch>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace zenthrildb;
namespace {
std::string bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read lifecycle fixture");
  return {std::istreambuf_iterator<char>(stream), {}};
}
void signal(const std::filesystem::path& path) {
  std::ofstream stream(path, std::ios::binary); stream << "ready"; stream.close();
  if (!stream) throw std::runtime_error("Cannot signal lifecycle barrier");
}
void waitSignal(const std::filesystem::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  while (!std::filesystem::exists(path)) {
    if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Lifecycle barrier timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
template<class F> void rejects(F&& operation) {
  bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
}
struct Child {
#ifdef _WIN32
  HANDLE handle{};
#else
  pid_t pid{};
#endif
};
Child start(const char* executable, const std::string& action, const std::filesystem::path& source,
            const std::filesystem::path& candidate, const std::filesystem::path& control) {
#ifdef _WIN32
  (void)executable;
  std::wstring program(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
  if (!size || size >= program.size()) throw std::runtime_error("Cannot find test executable");
  program.resize(size);
  auto command = L"\"" + program + L"\" --ownership-child " + std::wstring(action.begin(), action.end()) +
    L" \"" + source.wstring() + L"\" \"" + candidate.wstring() + L"\" \"" + control.wstring() + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start ownership child");
  CloseHandle(process.hThread); return Child{process.hProcess};
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Cannot fork ownership child");
  if (child == 0) {
    execl(executable, executable, "--ownership-child", action.c_str(), source.c_str(), candidate.c_str(), control.c_str(), nullptr);
    _exit(127);
  }
  return Child{child};
#endif
}
int finish(Child& child, bool terminate = false) {
#ifdef _WIN32
  if (terminate && !TerminateProcess(child.handle, 91)) throw std::runtime_error("Cannot terminate owner");
  if (WaitForSingleObject(child.handle, 45000) != WAIT_OBJECT_0) {
    TerminateProcess(child.handle, 3); WaitForSingleObject(child.handle, 5000); CloseHandle(child.handle);
    throw std::runtime_error("Ownership child timeout");
  }
  DWORD status{}; const auto read = GetExitCodeProcess(child.handle, &status); CloseHandle(child.handle);
  if (!read) throw std::runtime_error("Cannot read child status");
  return static_cast<int>(status);
#else
  if (terminate && kill(child.pid, SIGKILL) != 0) throw std::runtime_error("Cannot terminate owner");
  int status{}; const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  for (;;) {
    const auto result = waitpid(child.pid, &status, WNOHANG);
    if (result == child.pid) break;
    if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for ownership child");
    if (std::chrono::steady_clock::now() > deadline) {
      kill(child.pid, SIGKILL); while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Ownership child timeout");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (terminate && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) return 91;
  if (!WIFEXITED(status)) throw std::runtime_error("Unexpected owner termination");
  return WEXITSTATUS(status);
#endif
}
}

int runOwnershipChild(const char* actionArg, const char* sourceArg, const char* candidateArg, const char* controlArg) {
  const std::string action(actionArg);
  const std::filesystem::path source(sourceArg), candidate(candidateArg), control(controlArg);
  try {
    if (action.starts_with("race-")) {
      const auto id = action.substr(5); signal(control / ("ready-" + id)); waitSignal(control / "go");
      DatabaseRuntime db(source); signal(control / ("owned-" + id)); waitSignal(control / "release"); return 0;
    }
    if (action.starts_with("install-")) {
      const auto id = action.substr(8); signal(control / ("ready-" + id)); waitSignal(control / "go");
      try { DatabaseRuntime::installReduction(source, candidate); return 0; }
      catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) == "Another generation is already installed") return 42;
        throw;
      }
    }
    if (action == "reduce") { DatabaseRuntime::reduceWalTo(source, control / "reduced"); return 0; }
    if (action == "install") { DatabaseRuntime::installReduction(source, candidate); return 0; }
    if (action == "raw") { LogManager wal(databaseWalPath(source)); (void)wal.checkpoint(); return 0; }
    if (action == "recover") {
      FileManager files; files.openDatabase(source);
      LogManager wal(databaseWalPath(source), files.ownership());
      (void)RecoveryCoordinator{}.recover(wal, files); return 0;
    }
    DatabaseRuntime db(source);
    assert(db.records().get("records", "stable") == std::vector<Byte>{9});
    if (action == "checkpoint") { (void)db.checkpoint(); return 0; }
    if (action == "hold" || action == "hold-commit") {
      if (action == "hold-commit") db.records().put("records", "confirmed", std::vector<Byte>{7});
      signal(control / "ready"); waitSignal(control / "release");
    }
    return 0;
  } catch (const DatabaseBusyError&) { return 41; }
}

void test_database_lifecycle(const char* executable) {
  const auto root = std::filesystem::temp_directory_path() /
    ("zdb-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(root));
  const auto source = root / "sample.zdb";
  { DatabaseRuntime db(source, true); (void)db.createTable("records"); db.records().put("records", "stable", std::vector<Byte>{9}); }
  const auto original = bytes(source), originalWal = bytes(databaseWalPath(source));
#ifdef _WIN32
  {
    const auto longFile = root / "database-with-a-long-filename.zdb";
    std::filesystem::copy_file(source, longFile);
    std::filesystem::copy_file(databaseWalPath(source), databaseWalPath(longFile));
    std::wstring shortName(32768, L'\0');
    const auto size = GetShortPathNameW(longFile.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (size && size < shortName.size()) {
      shortName.resize(size); const std::filesystem::path alias(shortName);
      if (alias.filename() != longFile.filename()) rejects([&] { FileManager files; files.openDatabase(alias); });
      else std::cout << "SKIP Windows 8.3 alias: filesystem supplied no distinct filename\n";
    } else std::cout << "SKIP Windows 8.3 alias: short pathname unavailable\n";
    DatabaseRuntime valid(longFile);
  }
#endif
  {
    FileManager files; files.openDatabase(source);
    rejects([&] { FileManager other; other.openDatabase(source); });
    rejects([&] { LogManager raw(databaseWalPath(source)); });
    LogManager wal(databaseWalPath(source), files.header(), files.walIdentity(), false, files.ownership());
    rejects([&] { LogManager duplicate(databaseWalPath(source), files.ownership()); });
    rejects([&] { files.closeDatabase(); });
    std::latch held(1), release(1); std::exception_ptr workerError;
    std::thread worker([&] { try { auto operation = files.operation(); held.count_down(); release.wait(); }
      catch (...) { workerError = std::current_exception(); held.count_down(); } });
    held.wait();
    rejects([&] { (void)RecoveryCoordinator{}.recover(wal, files); });
    rejects([&] { auto page = files.readPage(kDatabaseMetadataPageId); files.writePage(page); });
    rejects([&] { files.closeDatabase(); });
    release.count_down(); worker.join(); if (workerError) std::rethrow_exception(workerError);
    (void)RecoveryCoordinator{}.recover(wal, files);
    wal.close(); files.closeDatabase();
    assert(!files.isOpen());
    rejects([&] { (void)RecoveryCoordinator{}.recover(wal, files); });
  }
  {
    FileManager files; files.openDatabase(source);
    { BufferManager cache(files, 2); rejects([&] { BufferManager second(files, 2); }); rejects([&] { files.closeDatabase(); }); }
    files.closeDatabase();
  }
  {
    auto files = std::make_unique<FileManager>(); files->openDatabase(source);
    auto wal = std::make_unique<LogManager>(databaseWalPath(source), files->ownership());
    files.reset();
    rejects([&] { (void)wal->checkpoint(); });
    rejects([&] { DatabaseRuntime other(source); });
    wal.reset(); DatabaseRuntime reopened(source);
  }
  assert(bytes(source) == original && bytes(databaseWalPath(source)) == originalWal);
  DatabaseRuntime::reduceWalTo(source, root / "candidate");
  const auto candidate = root / "candidate" / source.filename();
  int sequence = 0;
  const auto controls = [&] { const auto path = root / ("control-" + std::to_string(sequence++)); assert(std::filesystem::create_directory(path)); return path; };
  for (bool kill : {false, true}) {
    const auto control = controls(); auto owner = start(executable, kill ? "hold-commit" : "hold", source, candidate, control);
    waitSignal(control / "ready");
    const auto before = bytes(source), walBefore = bytes(databaseWalPath(source));
    for (const std::string action : {"probe", "reduce", "install", "raw", "recover", "checkpoint"}) {
      auto rejected = start(executable, action, source, candidate, control); assert(finish(rejected) == 41);
      assert(bytes(source) == before && bytes(databaseWalPath(source)) == walBefore);
    }
    if (!kill) signal(control / "release");
    assert(finish(owner, kill) == (kill ? 91 : 0));
    auto next = start(executable, "probe", source, candidate, control); assert(finish(next) == 0);
    DatabaseRuntime db(source);
    if (kill) assert(db.records().get("records", "confirmed") == std::vector<Byte>{7});
  }
  {
    const auto control = controls();
    auto a = start(executable, "race-A", source, candidate, control), b = start(executable, "race-B", source, candidate, control);
    waitSignal(control / "ready-A"); waitSignal(control / "ready-B"); signal(control / "go");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (!std::filesystem::exists(control / "owned-A") && !std::filesystem::exists(control / "owned-B")) {
      if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("No ownership race winner");
      std::this_thread::yield();
    }
    const bool aWon = std::filesystem::exists(control / "owned-A");
    assert(finish(aWon ? b : a) == 41);
    assert(!std::filesystem::exists(control / (aWon ? "owned-B" : "owned-A")));
    signal(control / "release"); assert(finish(aWon ? a : b) == 0);
  }
  DatabaseRuntime::reduceWalTo(source, root / "fresh-A"); DatabaseRuntime::reduceWalTo(source, root / "fresh-B");
  const auto targetA = root / "fresh-A" / source.filename(), targetB = root / "fresh-B" / source.filename();
  const auto preserved = bytes(source), preservedWal = bytes(databaseWalPath(source));
  {
    const auto control = controls();
    auto a = start(executable, "install-A", source, targetA, control), b = start(executable, "install-B", source, targetB, control);
    waitSignal(control / "ready-A"); waitSignal(control / "ready-B"); signal(control / "go");
    const auto one = finish(a), two = finish(b);
    assert((one == 0 || one == 41 || one == 42) && (two == 0 || two == 41 || two == 42));
    const auto state = readWalInstallation(source);
    assert(state.targetPath == targetA || state.targetPath == targetB);
    DatabaseRuntime db(source); assert(db.records().get("records", "confirmed") == std::vector<Byte>{7});
    auto direct = start(executable, "probe", state.targetPath, candidate, control); assert(finish(direct) == 41);
    auto alias = start(executable, "probe", source, candidate, control); assert(finish(alias) == 41);
  }
  assert(bytes(source) == preserved && bytes(databaseWalPath(source)) == preservedWal);
  rejects([&] { LogManager ignoredSelector(databaseWalPath(source)); });
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto failureDb = root / "failure.zdb";
  { DatabaseRuntime db(failureDb, true); }
  for (const auto point : {IoFaultPoint::OwnershipAcquire, IoFaultPoint::OwnershipAcquired}) {
    const auto before = bytes(failureDb), wal = bytes(databaseWalPath(failureDb));
    IoFaultInjector::arm(point); rejects([&] { FileManager files; files.openDatabase(failureDb); }); IoFaultInjector::clear();
    assert(bytes(failureDb) == before && bytes(databaseWalPath(failureDb)) == wal);
    DatabaseRuntime reopened(failureDb);
  }
  for (const auto point : {IoFaultPoint::DatabaseClose, IoFaultPoint::OwnershipRelease, IoFaultPoint::PageSync}) {
    FileManager files; files.openDatabase(failureDb);
    IoFaultInjector::arm(point); rejects([&] { files.closeDatabase(); }); IoFaultInjector::clear();
    assert(!files.isOpen()); DatabaseRuntime reopened(failureDb);
  }
#endif
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end()); });
  for (const auto& path : paths) std::filesystem::remove(path);
  std::filesystem::remove(root);
}
