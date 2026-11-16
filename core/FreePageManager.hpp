#pragma once

#include "core/FileManager.hpp"
#include "storage/FreePageList.hpp"
#include "transaction/StorageMutationCoordinator.hpp"

#include <mutex>
#include <optional>
#include <unordered_set>

namespace zenthrildb {

class FreePageManager {
public:
  FreePageManager(FileManager& fileManager, StorageMutationCoordinator& mutationCoordinator);

  [[nodiscard]] PageId allocatePage();
  [[nodiscard]] PageId allocatePageDeferred();
  void completeDeferredAllocation(PageId pageId);
  void freePage(PageId pageId);
  void freePageDeferred(PageId pageId);
  void load();
  void save();
  [[nodiscard]] Page snapshotFreeListPage() const;
  void checkFreeListCapacity(std::size_t additionalPages) const;
  [[nodiscard]] const FreePageList& freePages() const noexcept { return freePages_; }

private:
  friend class SnapshotBaseline;
  explicit FreePageManager(FileManager& fileManager) : fileManager_(fileManager) {}
  [[nodiscard]] StorageMutationCoordinator& mutations() const;
  Page buildFreePage() const;
  Page serializeFreeListPage() const;
  void persistFreeListToDisk();
  void loadFreeListFromDisk();
  void checkFreeListCapacityLocked(std::size_t additionalPages) const;

  FileManager& fileManager_;
  StorageMutationCoordinator* mutationCoordinator_{};
  mutable std::mutex mutex_;
  FreePageList freePages_;
  std::unordered_set<PageId> pendingAllocations_;
};

} // namespace zenthrildb
