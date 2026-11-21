#include "core/BufferManager.hpp"
#include "core/FileManager.hpp"
#include "tests/TestDatabaseFiles.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_buffer_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_buffer_test.zdb";
  removeTestDatabaseArtifacts(path);

  FileManager fm;
  fm.createDatabase(path);

  Page page;
  page.header().pageId = kDatabaseMetadataPageId;
  page.header().pageType = PageType::Metadata;
  page.header().usedBytes = 4;
  fm.writePage(page);

  {
  BufferManager buffer(fm, 2);
  auto& cached = buffer.pin(kDatabaseMetadataPageId);
  assert(cached.header().pageId == kDatabaseMetadataPageId);
  assert(cached.header().pageType == PageType::Metadata);
  buffer.unpin(kDatabaseMetadataPageId);
  buffer.flushAll();

  {
    auto guard = buffer.pinGuard(kDatabaseMetadataPageId);
    assert(guard.valid());
    assert(guard.page().header().pageId == kDatabaseMetadataPageId);
    guard.page().header().usedBytes = 8;
    guard.markDirty();
  }

  Page another;
  another.header().pageId = kPageDirectoryPageId;
  another.header().pageType = PageType::Metadata;
  fm.writePage(another);

  Page third;
  third.header().pageId = kFirstUserPageId;
  third.header().pageType = PageType::Metadata;
  fm.writePage(third);
  }

  {
  BufferManager smallBuffer(fm, 1);
  {
    auto guard = smallBuffer.pinGuard(kDatabaseMetadataPageId);
    assert(guard.page().header().pageId == kDatabaseMetadataPageId);
  }
  auto secondGuard = smallBuffer.pinGuard(kPageDirectoryPageId);
  assert(secondGuard.page().header().pageId == kPageDirectoryPageId);
  }

  fm.closeDatabase();
  removeTestDatabaseArtifacts(path);
}
