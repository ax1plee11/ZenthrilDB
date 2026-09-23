#include "core/FreePageManager.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/BinaryFormat.hpp"

#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {

FreePageManager::FreePageManager(FileManager& fileManager)
  : fileManager_(fileManager) {}

PageId FreePageManager::allocatePage() {
  std::lock_guard lock(mutex_);
  if (auto recycled = freePages_.acquire()) {
    return *recycled;
  }
  return fileManager_.reservePageId();
}

void FreePageManager::freePage(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (pageId < kFirstUserPageId) {
    throw std::invalid_argument("Reserved page cannot be added to free page list");
  }
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

Page FreePageManager::buildFreePage() const {
  Page page;
  page.header().pageType = PageType::Free;
  page.header().usedBytes = 0;
  return page;
}

void FreePageManager::persistFreeListToDisk() {
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
  fileManager_.writePage(page);
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
