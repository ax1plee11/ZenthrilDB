#include "core/FileManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_file_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_test.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);
  assert(fm.isOpen());
  fm.closeDatabase();
  assert(!fm.isOpen());
  std::filesystem::remove(path);
}
