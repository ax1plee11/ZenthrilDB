#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/PageManager.hpp"

#include <cassert>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace zenthrildb;

void test_page_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_pagemgr.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);
  FreePageManager freePages(fm);
  freePages.load();
  PageManager pages(fm, freePages);

  auto page = pages.allocatePage(PageType::Table, "accounts");
  assert(page.header().pageId >= kFirstUserPageId);
  assert(pages.isDirty(page.header().pageId));
  const auto allocatedInfo = pages.pageInfo(page.header().pageId);
  assert(allocatedInfo.has_value());
  assert(allocatedInfo->ownerTable == "accounts");
  assert(allocatedInfo->status == PageStatus::InUse);

  page.payload()[0] = 42;
  pages.writePage(page);
  assert(pages.isDirty(page.header().pageId));
  auto restored = pages.readPage(page.header().pageId);
  assert(restored.payload()[0] == 42);
  assert(pages.validatePage(page.header().pageId));

  pages.flushDirtyPages();
  assert(!pages.isDirty(page.header().pageId));

  pages.freePage(page.header().pageId);
  const auto freeInfo = pages.pageInfo(page.header().pageId);
  assert(freeInfo.has_value());
  assert(freeInfo->status == PageStatus::Free);

  auto reused = pages.allocatePage(PageType::Index, "accounts_idx");
  assert(reused.header().pageId == page.header().pageId);

  bool rejectedReservedWrite = false;
  try {
    Page reserved;
    reserved.header().pageId = kDatabaseMetadataPageId;
    reserved.header().pageType = PageType::Metadata;
    pages.writePage(reserved);
  } catch (const std::runtime_error&) {
    rejectedReservedWrite = true;
  }
  assert(rejectedReservedWrite);

  std::vector<PageId> allocated;
  std::mutex guard;
  std::thread t1([&] {
    auto p = pages.allocatePage(PageType::Table);
    std::lock_guard lock(guard);
    allocated.push_back(p.header().pageId);
  });
  std::thread t2([&] {
    auto p = pages.allocatePage(PageType::Index);
    std::lock_guard lock(guard);
    allocated.push_back(p.header().pageId);
  });
  t1.join();
  t2.join();
  assert(allocated.size() == 2);
  assert(allocated[0] != allocated[1]);

  fm.closeDatabase();
  std::filesystem::remove(path);
}
