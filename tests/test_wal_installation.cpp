#include "core/DatabaseRuntime.hpp"
#include "storage/IoFaultInjector.hpp"
#include "storage/WalInstallation.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
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
std::string read(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return "missing";
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read installation fixture");
  return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), bytes.size()); stream.close();
  if (!stream) throw std::runtime_error("Cannot write installation fixture");
}
std::vector<std::string> image(const std::filesystem::path& path) {
  return {read(path), read(databaseWalPath(path)), read(snapshotBaselinePath(path)), read(snapshotManifestPath(path))};
}
void copySet(const std::filesystem::path& from, const std::filesystem::path& to) {
  for (const auto& pair : {std::pair{from, to}, std::pair{databaseWalPath(from), databaseWalPath(to)},
      std::pair{snapshotBaselinePath(from), snapshotBaselinePath(to)}, std::pair{snapshotManifestPath(from), snapshotManifestPath(to)}}) {
    if (std::filesystem::exists(pair.first)) std::filesystem::copy_file(pair.first, pair.second);
  }
}
template<class F> void rejects(F&& operation) {
  bool rejected = false;
  try { operation(); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
}
void verifyData(const std::filesystem::path& source) {
  for (int i = 0; i < 2; ++i) {
    DatabaseRuntime db(source);
    assert(db.listTables().size() == 2);
    assert(db.records().get("notes", "keep") == (std::vector<Byte>{0, 255, 7}));
    assert(!db.records().get("notes", "gone"));
    assert(db.records().get("empty", "nil") == std::vector<Byte>{});
    assert(db.records().scan("notes").size() == 1);
    (void)db.checkConsistency();
  }
}
struct Phase { const char* name; IoFaultPoint point; bool committed; };
constexpr Phase phases[] = {
  {"prepared", IoFaultPoint::InstallPrepared, false}, {"validated", IoFaultPoint::InstallValidated, false},
  {"created", IoFaultPoint::InstallCreated, false}, {"write", IoFaultPoint::InstallWrite, false},
  {"partial", IoFaultPoint::InstallPartial, false}, {"flush", IoFaultPoint::InstallFlush, false},
  {"sync", IoFaultPoint::InstallSync, false}, {"durable", IoFaultPoint::InstallDurable, false},
  {"rename", IoFaultPoint::InstallRename, false}, {"switched", IoFaultPoint::InstallSwitched, true},
  {"directory-sync", IoFaultPoint::InstallDirectorySync, true},
  {"parent-durable", IoFaultPoint::InstallParentDurable, true}, {"open", IoFaultPoint::InstallOpen, true},
  {"verified", IoFaultPoint::InstallVerified, true},
};
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
void crash(const char* executable, const std::filesystem::path& source,
           const std::filesystem::path& target, const char* phase) {
#ifdef _WIN32
  (void)executable;
  std::wstring program(32768, L'\0');
  const auto count = GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
  if (!count || count >= program.size()) throw std::runtime_error("Cannot resolve test executable");
  program.resize(count);
  const std::string text(phase);
  auto command = L"\"" + program + L"\" --install-child \"" + source.wstring() + L"\" \"" +
    target.wstring() + L"\" " + std::wstring(text.begin(), text.end());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start installation crash child");
  CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, 45000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess); throw std::runtime_error("Installation child timeout");
  }
  DWORD code{}; const bool received = GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  if (!received || code != 91) throw std::runtime_error("Installation child missed crash point");
#else
  const auto child = fork();
  if (child < 0) throw std::runtime_error("Cannot fork installation child");
  if (child == 0) { execl(executable, executable, "--install-child", source.c_str(), target.c_str(), phase, nullptr); _exit(127); }
  int status{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  for (;;) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) break;
    if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for installation child");
    if (std::chrono::steady_clock::now() > deadline) {
      kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Installation child timeout");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 91) throw std::runtime_error("Installation child missed crash point");
#endif
}
#endif
}

int runInstallationChild(const char* source, const char* target, const char* phase) {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  if (std::string_view(phase) == "recovery") {
    IoFaultInjector::armCrash(IoFaultPoint::RecoveryPageRepair);
    DatabaseRuntime db(source);
    return 2;
  }
  for (const auto& item : phases) if (item.name == std::string_view(phase)) {
    IoFaultInjector::armCrash(item.point);
    DatabaseRuntime::installReduction(source, target);
    return 2;
  }
#else
  (void)source; (void)target; (void)phase;
#endif
  return 2;
}

void test_wal_installation(const char* executable) {
  const auto root = std::filesystem::temp_directory_path() /
    ("zdb-install-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(root));
  const auto initial = root / "sample.zdb";
  {
    DatabaseRuntime db(initial, true);
    (void)db.createTable("notes"); (void)db.createTable("empty");
    db.records().put("notes", "keep", std::vector<Byte>{1});
    db.records().put("notes", "keep", std::vector<Byte>{0, 255, 7});
    db.records().put("notes", "gone", std::vector<Byte>{2});
    assert(db.records().erase("notes", "gone"));
    db.records().put("empty", "nil", std::vector<Byte>{});
    (void)db.checkpoint();
  }
  DatabaseRuntime::snapshotTo(initial, root / "seed", true);
  const auto seed = root / "seed" / "sample.zdb";
  {
    FileManager files; files.openDatabase(seed);
    LogManager wal(databaseWalPath(seed), files.header(), files.walIdentity(), false, files.ownership());
    TransactionManager tx(wal); (void)tx.begin();
  }
  DatabaseRuntime::reduceWalTo(seed, root / "candidate-seed");
  const auto targetSeed = root / "candidate-seed" / "sample.zdb";
  int fixtureNumber = 0;
  const auto fixture = [&] {
    const auto directory = root / ("case-" + std::to_string(fixtureNumber++));
    assert(std::filesystem::create_directory(directory));
    assert(std::filesystem::create_directory(directory / "candidate"));
    const auto source = directory / "sample.zdb";
    const auto target = directory / "candidate" / "sample.zdb";
    copySet(seed, source); copySet(targetSeed, target);
    return std::pair{source, target};
  };
  {
    const auto [source, target] = fixture();
    const auto before = image(source);
    {
      DatabaseRuntime busy(source);
      rejects([&] { DatabaseRuntime::installReduction(source, target); });
    }
    DatabaseRuntime::installReduction(source, target);
    verifyData(source);
    assert(image(source) == before);
    const auto state = readWalInstallation(source);
    const auto encoded = state.serialize();
    assert(WalInstallation::deserialize(encoded).target == state.target);
    for (std::size_t size = 0; size < encoded.size(); ++size) {
      rejects([&] { (void)WalInstallation::deserialize(std::span<const Byte>(encoded).first(size)); });
    }
    for (std::size_t offset = 0; offset < encoded.size(); ++offset) {
      auto damaged = encoded; damaged[offset] ^= 1;
      rejects([&] { (void)WalInstallation::deserialize(damaged); });
    }
    assert(state.sourceHasBaseline);
    const auto alternative = source.parent_path() / "alternative";
    assert(std::filesystem::create_directory(alternative));
    copySet(targetSeed, alternative / "sample.zdb");
    rejects([&] { DatabaseRuntime::installReduction(source, alternative / "sample.zdb"); });
    DatabaseRuntime::installReduction(source, target);
    {
      DatabaseRuntime db(source);
      db.records().put("notes", "after", std::vector<Byte>{9});
      assert(db.checkpoint().transactionHighWater > 0);
      db.backupTo(source.parent_path() / "backup");
      rejects([&] { DatabaseRuntime direct(target); });
    }
    assert(image(source) == before);
    const auto stable = image(target);
    for (int i = 0; i < 2; ++i) {
      DatabaseRuntime db(source);
      assert(db.records().get("notes", "after") == std::vector<Byte>{9});
    }
    assert(image(target) == stable);
    DatabaseRuntime::installReduction(source, target); // Retains post-install commits.
    (void)DatabaseRuntime::verifySnapshot(source);
    DatabaseRuntime::reduceWalTo(source, source.parent_path() / "next");
    DatabaseRuntime reopened(source.parent_path() / "next" / "sample.zdb");
    assert(reopened.records().get("notes", "after") == std::vector<Byte>{9});
  }
  {
    const auto [source, target] = fixture();
    { DatabaseRuntime db(source); db.records().put("notes", "newer", std::vector<Byte>{3}); }
    const auto before = image(source);
    rejects([&] { DatabaseRuntime::installReduction(source, target); });
    assert(image(source) == before && !std::filesystem::exists(walInstallationPath(source)));
  }
  for (int failure = 0; failure < 18; ++failure) {
    const auto [source, target] = fixture();
    DatabaseRuntime::installReduction(source, target);
    const auto marker = walInstallationPath(source);
    if (failure < 3) {
      auto data = read(marker);
      if (failure == 0) data.back() ^= 1;
      else if (failure == 1) data.resize(data.size() / 2);
      else data += 'x';
      write(marker, data);
    } else if (failure == 3) { auto data = read(databaseWalPath(target)); data.back() ^= 1; write(databaseWalPath(target), data); }
    else if (failure == 4) std::filesystem::remove(target);
    else if (failure == 5) std::filesystem::remove(databaseWalPath(source));
    else if (failure == 6) { auto data = read(snapshotManifestPath(target)); data.back() ^= 1; write(snapshotManifestPath(target), data); }
    else if (failure == 7 || failure == 8) {
      auto state = readWalInstallation(source);
      if (failure == 7) state.target.generation[0] ^= 1;
      else ++state.target.snapshotLsn;
      const auto data = state.serialize();
      write(marker, {reinterpret_cast<const char*>(data.data()), data.size()});
    } else if (failure == 9) std::filesystem::remove(snapshotBaselinePath(target));
    else if (failure == 10) std::filesystem::remove(source);
    else if (failure == 11) write(walInstallationPath(target), "conflict");
    else if (failure == 12) {
      auto state = readWalInstallation(source); state.targetPath = target.parent_path() / "other.zdb";
      const auto data = state.serialize(); write(marker, {reinterpret_cast<const char*>(data.data()), data.size()});
    } else if (failure == 13) write(databaseWalPath(source), read(databaseWalPath(source)) + 'x');
    else if (failure == 14) std::filesystem::remove(snapshotBaselinePath(source));
    else if (failure == 15) std::filesystem::remove(snapshotManifestPath(source));
    else if (failure == 16) std::filesystem::remove(databaseWalPath(target));
    else std::filesystem::resize_file(databaseWalPath(target), WalIdentity::kSize + 10);
    const auto sourceBefore = image(source), targetBefore = image(target);
    const auto markerBefore = read(marker);
    rejects([&] { DatabaseRuntime db(source); });
    rejects([&] { (void)DatabaseRuntime::verifySnapshot(source); });
    rejects([&] { DatabaseRuntime::installReduction(source, target); });
    assert(image(source) == sourceBefore && image(target) == targetBefore && read(marker) == markerBefore);
  }
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  {
    const auto [source, target] = fixture();
    const auto before = image(source), reduced = image(target);
    IoFaultInjector::arm(IoFaultPoint::InstallSync, 5); // Four candidate files, then selector fsync.
    bool injected = false;
    try { DatabaseRuntime::installReduction(source, target); }
    catch (const std::system_error& error) { injected = error.code() == std::errc::io_error; }
    IoFaultInjector::clear();
    assert(injected && image(source) == before && image(target) == reduced);
    assert(!std::filesystem::exists(walInstallationPath(source)));
    DatabaseRuntime::installReduction(source, target); verifyData(source);
  }
  {
    const auto [source, target] = fixture();
    const auto before = image(source), expected = image(target);
    DatabaseRuntime::installReduction(source, target);
    const auto marker = read(walInstallationPath(source));
    std::filesystem::resize_file(target, DatabaseHeader::kSerializedSize + kDefaultPageSize);
    crash(executable, source, target, "recovery");
    crash(executable, source, target, "recovery");
    verifyData(source);
    assert(image(source) == before && image(target) == expected && read(walInstallationPath(source)) == marker);
  }
  {
    const auto [source, target] = fixture();
    const auto before = image(source);
    IoFaultInjector::arm(IoFaultPoint::NoSpace);
    bool injected = false;
    try { DatabaseRuntime::installReduction(source, target); }
    catch (const std::system_error& error) { injected = error.code() == std::errc::no_space_on_device; }
    IoFaultInjector::clear();
    assert(injected && image(source) == before && !std::filesystem::exists(walInstallationPath(source)));
    verifyData(source);
  }
  for (const auto& phase : phases) for (bool terminate : {false, true}) {
    const auto [source, target] = fixture();
    const auto original = image(source), reduced = image(target);
    if (terminate) crash(executable, source, target, phase.name);
    else {
      IoFaultInjector::arm(phase.point);
      bool injected = false;
      try { DatabaseRuntime::installReduction(source, target); }
      catch (const std::system_error& error) { injected = error.code() == std::errc::io_error; }
      IoFaultInjector::clear(); assert(injected);
    }
    assert(std::filesystem::exists(walInstallationPath(source)) == phase.committed);
    assert(image(source) == original && image(target) == reduced);
    verifyData(source);
    DatabaseRuntime::installReduction(source, target);
    verifyData(source);
    assert(image(source) == original && image(target) == reduced);
  }
  {
    const auto [source, target] = fixture();
    IoFaultInjector::arm(IoFaultPoint::InstallPartial);
    IoFaultInjector::arm(IoFaultPoint::InstallCleanup);
    rejects([&] { DatabaseRuntime::installReduction(source, target); });
    IoFaultInjector::clear(); verifyData(source);
    assert(!std::filesystem::exists(walInstallationPath(source)));
    DatabaseRuntime::installReduction(source, target); verifyData(source);
  }
#else
  (void)executable;
#endif
  // Remove only files created under this unique fixture root; never recurse over user data.
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
    return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
  });
  for (const auto& path : paths) std::filesystem::remove(path);
  std::filesystem::remove(root);
}
