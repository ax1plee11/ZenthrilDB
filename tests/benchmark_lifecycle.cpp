#include "core/DatabaseRuntime.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
using namespace zenthrildb;
int benchmark_lifecycle(const char* directory) {
  const auto root = std::filesystem::absolute(directory);
  if (!std::filesystem::create_directory(root)) throw std::runtime_error("Benchmark requires a new directory");
  const auto path = root / "sample.zdb";
  std::map<std::string, double> samples;
  const auto measure = [&](const std::string& name, auto operation) {
    const auto start = std::chrono::steady_clock::now(); operation();
    samples[name] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  };
  { DatabaseRuntime runtime(path, true); (void)runtime.createTable("records"); }
  for (int repeat = 0; repeat < 3; ++repeat) {
    FileManager files;
    measure("file_open_ms_sum3", [&] { files.openDatabase(path); });
    measure("file_close_ms_sum3", [&] { files.closeDatabase(); });
    measure("ownership_acquire_release_ms_sum3", [&] { DatabaseFileLock ownership(path); });
  }
  {
    DatabaseRuntime runtime(path);
    measure("ten_transactions_ms", [&] {
      for (int record = 0; record < 10; ++record) runtime.records().put("records", std::to_string(record), std::vector<Byte>{1, 2, 3});
    });
    measure("checkpoint_ms", [&] { (void)runtime.checkpoint(); });
  }
  {
    LogManager standalone(root / "standalone.wal");
    measure("ten_durable_wal_appends_ms", [&] {
      for (int record = 0; record < 10; ++record) (void)standalone.checkpoint();
    });
  }
  measure("reduction_ms", [&] { DatabaseRuntime::reduceWalTo(path, root / "reduced"); });
  const auto candidate = root / "reduced" / path.filename();
  measure("installation_ms", [&] { DatabaseRuntime::installReduction(path, candidate); });
  for (int repeat = 0; repeat < 3; ++repeat) {
    measure("installed_runtime_reopen_close_ms_sum3", [&] {
      DatabaseRuntime runtime(path);
      if (runtime.records().scan("records").size() != 10) throw std::runtime_error("Benchmark lost records");
    });
  }
  std::cout << '{'; bool first = true;
  for (const auto& [key, value] : samples) {
    if (!first) std::cout << ','; first = false;
    std::cout << '"' << key << "\":" << value;
  }
  std::cout << "}\n"; return 0;
}
