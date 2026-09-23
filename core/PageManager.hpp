#pragma once

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "storage/PageDirectory.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace zenthrildb {

class PageManager {
public:
  PageManager(FileManager& fileManager, FreePageManager& freePageManager);

  [[nodiscard]] Page allocatePage(PageType type);
  [[nodiscard]] Page allocatePage(PageType type, std::string owner);
  void freePage(PageId pageId);
  [[nodiscard]] Page readPage(PageId pageId);
  void writePage(const Page& page);
  void flushDirtyPages();
  [[nodiscard]] bool validatePage(PageId pageId);
  [[nodiscard]] bool isDirty(PageId pageId) const;
  [[nodiscard]] std::optional<PageDirectoryEntry> pageInfo(PageId pageId) const;

private:
  [[nodiscard]] PageDirectoryEntry makeDirectoryEntry(const Page& page, std::string owner, PageStatus status) const;

  mutable std::mutex mutex_;
  FileManager& fileManager_;
  FreePageManager& freePageManager_;
  PageDirectory pageDirectory_;
  std::unordered_set<PageId> dirtyPages_;
};

} // namespace zenthrildb
