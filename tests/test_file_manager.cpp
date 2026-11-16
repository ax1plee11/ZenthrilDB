#include "core/FileManager.hpp"
#include "storage/DatabaseWalPath.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_file_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_test.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  std::filesystem::remove(databaseWalPath(path));
  auto initialLockPath = path;
  initialLockPath += ".lock";
  std::filesystem::remove(initialLockPath);

  FileManager fm;
  fm.createDatabase(path);
  assert(fm.isOpen());

  Page v2Page;
  v2Page.header().version = Page::kFormatVersionV2;
  v2Page.header().pageId = kFirstUserPageId;
  v2Page.header().pageType = PageType::Table;
  v2Page.header().usedBytes = 2;
  v2Page.payload()[0] = 0xA1;
  v2Page.payload()[1] = 0xB2;
  v2Page.setPageLsn(0);
  fm.writePage(v2Page);
  fm.flush();
  const auto restored = fm.readPage(kFirstUserPageId);
  assert(restored.header().version == Page::kFormatVersionV2);
  assert(restored.pageLsn() == 0);
  assert(restored.payload()[0] == 0xA1);
  assert(restored.payload()[1] == 0xB2);

  FileManager competing;
  bool concurrentOpenRejected = false;
  try { competing.openDatabase(path); }
  catch (const std::runtime_error&) { concurrentOpenRejected = true; }
  assert(concurrentOpenRejected);

  fm.closeDatabase();
  assert(!fm.isOpen());
  competing.openDatabase(path);
  competing.closeDatabase();
  std::filesystem::remove(path);
  std::filesystem::remove(databaseWalPath(path));
  auto lockPath = path;
  lockPath += ".lock";
  std::filesystem::remove(lockPath);
}
