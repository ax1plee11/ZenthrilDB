#pragma once

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"
#include "storage/PageDirectory.hpp"

#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace zenthrildb {

class PageManager {
public:
  PageManager(FileManager& fileManager, FreePageManager& freePageManager, MetadataManager& metadataManager);

  [[nodiscard]] Page allocatePage(PageType type);
  [[nodiscard]] Page allocatePage(PageType type, std::string owner);
  void freePage(PageId pageId);
  void freePageAndWrite(PageId pageId, const Page& updatedPage);
  [[nodiscard]] Page readPage(PageId pageId);
  [[nodiscard]] std::uint64_t databasePageCount() const;
  void writePage(const Page& page);
  void flushDirtyPages();
  [[nodiscard]] bool validatePage(PageId pageId);
  [[nodiscard]] bool isDirty(PageId pageId) const;
  [[nodiscard]] std::optional<PageDirectoryEntry> pageInfo(PageId pageId) const;

private:
  friend class RecordManager;
  void allocatePageAndWrite(Page page, std::string owner, Page updatedPage,
                            const std::function<void(Page&, PageId)>& linkPage);
  [[nodiscard]] PageDirectoryEntry makeDirectoryEntry(const Page& page, std::string owner, PageStatus status) const;

  mutable std::mutex mutex_;
  FileManager& fileManager_;
  FreePageManager& freePageManager_;
  MetadataManager& metadataManager_;
  std::unordered_set<PageId> dirtyPages_;
};

} // namespace zenthrildb
