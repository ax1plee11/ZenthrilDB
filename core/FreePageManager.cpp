#include "core/FreePageManager.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/BinaryFormat.hpp"

#include <stdexcept>
#include <limits>
#include <algorithm>
#include <unordered_set>

namespace zenthrildb {

FreePageManager::FreePageManager(FileManager& fileManager, StorageMutationCoordinator& mutationCoordinator)
  : fileManager_(fileManager), mutationCoordinator_(&mutationCoordinator) {}

StorageMutationCoordinator& FreePageManager::mutations() const {
  if (!mutationCoordinator_) throw std::logic_error("Read-only free-page manager");
  return *mutationCoordinator_;
}

PageId FreePageManager::allocatePage() {
  std::lock_guard lock(mutex_);
  const auto beforeHeader = fileManager_.header();
  PageId pageId{};
  if (auto recycled = freePages_.acquire()) {
    pageId = *recycled;
  } else {
    if (beforeHeader.totalPages > std::numeric_limits<PageId>::max()) {
      throw std::overflow_error("Page identifier space exhausted");
    }
    pageId = static_cast<PageId>(beforeHeader.totalPages);
  }
  Page reserved;
  reserved.header().pageId = pageId;
  reserved.header().pageType = PageType::Reserved;
  const auto finalCount = std::max<std::uint64_t>(beforeHeader.totalPages,
    static_cast<std::uint64_t>(pageId) + 1);
  mutations().writePagesAtomically({reserved, serializeFreeListPage()}, beforeHeader, finalCount);
  return pageId;
}

PageId FreePageManager::allocatePageDeferred() {
  std::lock_guard lock(mutex_);
  PageId pageId{};
  if (auto recycled = freePages_.acquire()) {
    pageId = *recycled;
  } else {
    auto candidate = fileManager_.header().totalPages;
    while (candidate <= std::numeric_limits<PageId>::max() &&
           pendingAllocations_.contains(static_cast<PageId>(candidate))) {
      ++candidate;
    }
    if (candidate > std::numeric_limits<PageId>::max()) {
      throw std::overflow_error("Page identifier space exhausted");
    }
    pageId = static_cast<PageId>(candidate);
  }
  if (!pendingAllocations_.insert(pageId).second) {
    throw std::logic_error("Page id is already reserved by an unfinished storage mutation");
  }
  return pageId;
}

void FreePageManager::completeDeferredAllocation(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (pendingAllocations_.erase(pageId) != 1) {
    throw std::logic_error("Page id has no pending allocation");
  }
}

void FreePageManager::freePage(PageId pageId) {
  freePageDeferred(pageId);
  Page page;
  page.header().pageId = pageId;
  page.header().pageType = PageType::Free;
  std::lock_guard lock(mutex_);
  mutations().writePagesAtomically({page, serializeFreeListPage()});
}

void FreePageManager::freePageDeferred(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (pageId < kFirstUserPageId) {
    throw std::invalid_argument("Reserved page cannot be added to free page list");
  }
  if (!freePages_.contains(pageId)) checkFreeListCapacityLocked(1);
  freePages_.add(pageId);
}

void FreePageManager::load() {
  std::lock_guard lock(mutex_);
  loadFreeListFromDisk();
}

void FreePageManager::save() {
  std::lock_guard lock(mutex_);
  persistFreeListToDisk();
}

Page FreePageManager::snapshotFreeListPage() const {
  std::lock_guard lock(mutex_);
  return serializeFreeListPage();
}

void FreePageManager::checkFreeListCapacity(std::size_t additionalPages) const {
  std::lock_guard lock(mutex_);
  checkFreeListCapacityLocked(additionalPages);
}

void FreePageManager::checkFreeListCapacityLocked(std::size_t additionalPages) const {
  constexpr auto capacity = (Page::kV2PayloadSize - format::kFreePageListEntriesOffset) / sizeof(PageId);
  if (freePages_.size() > capacity || additionalPages > capacity - freePages_.size()) {
    throw std::length_error("Database free-page list is full");
  }
}

Page FreePageManager::buildFreePage() const {
  Page page;
  page.header().version = Page::kFormatVersionV2;
  page.header().pageType = PageType::Free;
  page.header().usedBytes = 0;
  return page;
}

void FreePageManager::persistFreeListToDisk() {
  mutations().writePagesAtomically({serializeFreeListPage()});
}

Page FreePageManager::serializeFreeListPage() const {
  auto page = buildFreePage();
  page.header().pageId = kFreePageListPageId;
  auto payload = page.payload();
  std::size_t offset = 0;
  const auto version = format::kFreePageListFormatVersion;
  const auto count = static_cast<std::uint32_t>(freePages_.size());
  if (payload.size() < format::kFreePageListEntriesOffset) {
    throw std::runtime_error("Free page list payload is too small");
  }
  binary::writeLittleEndian<std::uint32_t>(payload, offset, version);
  binary::writeLittleEndian<std::uint32_t>(payload, offset, count);
  for (auto pageId : freePages_) {
    if (offset + sizeof(pageId) > payload.size()) {
      throw std::runtime_error("Free page list exceeds payload size");
    }
    binary::writeLittleEndian<PageId>(payload, offset, pageId);
  }
  page.header().usedBytes = static_cast<std::uint32_t>(offset);
  return page;
}

void FreePageManager::loadFreeListFromDisk() {
  freePages_ = FreePageList{};
  if (fileManager_.header().totalPages <= kFreePageListPageId) {
    return;
  }
  auto page = fileManager_.readPage(kFreePageListPageId);
  const auto payload = page.payload();
  if (payload.size() < format::kFreePageListEntriesOffset) {
    throw std::runtime_error("Free page list payload is truncated");
  }
  auto offset = std::size_t{0};
  const auto version = binary::readLittleEndian<std::uint32_t>(payload, offset);
  if (version != 0 && version != format::kFreePageListFormatVersion) {
    throw std::invalid_argument("Unsupported free page list format version");
  }
  const auto count = binary::readLittleEndian<std::uint32_t>(payload, offset);
  if (count > (payload.size() - format::kFreePageListEntriesOffset) / sizeof(PageId)) {
    throw std::invalid_argument("Free page list count exceeds payload capacity");
  }
  std::unordered_set<PageId> seen;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (offset + sizeof(PageId) > payload.size()) {
      throw std::invalid_argument("Free page list is truncated");
    }
    const auto pageId = binary::readLittleEndian<PageId>(payload, offset);
    if (pageId < kFirstUserPageId || !seen.insert(pageId).second) {
      throw std::invalid_argument("Invalid or duplicate free page id");
    }
    freePages_.add(pageId);
  }
}

} // namespace zenthrildb
