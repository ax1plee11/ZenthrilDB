#pragma once

#include "core/FileManager.hpp"
#include "storage/FreePageList.hpp"

#include <mutex>
#include <optional>

namespace zenthrildb {

class FreePageManager {
public:
  explicit FreePageManager(FileManager& fileManager);

  [[nodiscard]] PageId allocatePage();
  void freePage(PageId pageId);
  void load();
  void save();
  [[nodiscard]] const FreePageList& freePages() const noexcept { return freePages_; }

private:
  Page buildFreePage() const;
  void persistFreeListToDisk();
  void loadFreeListFromDisk();

  FileManager& fileManager_;
  mutable std::mutex mutex_;
  FreePageList freePages_;
};

} // namespace zenthrildb
