#include "core/BufferManager.hpp"
#include "core/FileManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_buffer_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_buffer_test.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);

  Page page;
  page.header().pageId = 0;
  page.header().pageType = PageType::Metadata;
  page.header().usedBytes = 4;
  fm.writePage(page);

  BufferManager buffer(fm, 2);
  auto& cached = buffer.pin(0);
  assert(cached.header().pageId == 0);
  assert(cached.header().pageType == PageType::Metadata);
  buffer.unpin(0);
  buffer.flushAll();

  {
    auto guard = buffer.pinGuard(0);
    assert(guard.valid());
    assert(guard.page().header().pageId == 0);
    guard.page().header().usedBytes = 8;
    guard.markDirty();
  }

  Page another;
  another.header().pageId = 1;
  another.header().pageType = PageType::Metadata;
  fm.writePage(another);

  Page third;
  third.header().pageId = 2;
  third.header().pageType = PageType::Metadata;
  fm.writePage(third);

  BufferManager smallBuffer(fm, 1);
  {
    auto guard = smallBuffer.pinGuard(0);
    assert(guard.page().header().pageId == 0);
  }
  auto secondGuard = smallBuffer.pinGuard(1);
  assert(secondGuard.page().header().pageId == 1);

  fm.closeDatabase();
  std::filesystem::remove(path);
}
