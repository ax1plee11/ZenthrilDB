#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_free_page_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_freepages.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);

  FreePageManager freePages(fm);
  freePages.load();
  const auto freshPage = freePages.allocatePage();
  assert(freshPage >= kFirstUserPageId);
  freePages.freePage(7);
  freePages.save();

  FreePageManager reopened(fm);
  reopened.load();
  const auto reused = reopened.allocatePage();
  assert(reused == 7);

  fm.closeDatabase();
  std::filesystem::remove(path);
}
