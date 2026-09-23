#include "core/PageManager.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace zenthrildb {

PageManager::PageManager(FileManager& fileManager, FreePageManager& freePageManager)
  : fileManager_(fileManager), freePageManager_(freePageManager) {}

Page PageManager::allocatePage(PageType type) {
  return allocatePage(type, {});
}

Page PageManager::allocatePage(PageType type, std::string owner) {
  std::lock_guard lock(mutex_);
  Page page;
  page.header().pageId = freePageManager_.allocatePage();
  page.header().pageType = type;
  page.header().usedBytes = 0;
  pageDirectory_.registerPage(makeDirectoryEntry(page, std::move(owner), PageStatus::InUse));
  dirtyPages_.insert(page.header().pageId);
  return page;
}

void PageManager::freePage(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (pageId < kFirstUserPageId) {
    throw std::runtime_error("Reserved pages cannot be freed");
  }
  Page freePage;
  freePage.header().pageId = pageId;
  freePage.header().pageType = PageType::Free;
  freePage.header().usedBytes = 0;
  fileManager_.writePage(freePage);
  pageDirectory_.registerPage(makeDirectoryEntry(freePage, {}, PageStatus::Free));
  dirtyPages_.erase(pageId);
  freePageManager_.freePage(pageId);
}

Page PageManager::readPage(PageId pageId) {
  std::lock_guard lock(mutex_);
  auto page = fileManager_.readPage(pageId);
  if (pageDirectory_.get(pageId) == nullptr) {
    pageDirectory_.registerPage(makeDirectoryEntry(page, {}, PageStatus::InUse));
  }
  return page;
}

void PageManager::writePage(const Page& page) {
  std::lock_guard lock(mutex_);
  if (page.header().pageId < kFirstUserPageId) {
    throw std::runtime_error("Reserved pages must be managed by their owning subsystem");
  }
  fileManager_.writePage(page);
  pageDirectory_.registerPage(makeDirectoryEntry(page, {}, PageStatus::InUse));
  dirtyPages_.insert(page.header().pageId);
}

void PageManager::flushDirtyPages() {
  std::lock_guard lock(mutex_);
  fileManager_.flush();
  dirtyPages_.clear();
}

bool PageManager::validatePage(PageId pageId) {
  std::lock_guard lock(mutex_);
  try {
    (void)fileManager_.readPage(pageId);
    return true;
  } catch (...) {
    return false;
  }
}

bool PageManager::isDirty(PageId pageId) const {
  std::lock_guard lock(mutex_);
  return dirtyPages_.contains(pageId);
}

std::optional<PageDirectoryEntry> PageManager::pageInfo(PageId pageId) const {
  std::lock_guard lock(mutex_);
  if (const auto* entry = pageDirectory_.get(pageId)) {
    return *entry;
  }
  return std::nullopt;
}

PageDirectoryEntry PageManager::makeDirectoryEntry(const Page& page, std::string owner, PageStatus status) const {
  return PageDirectoryEntry{
    .pageId = page.header().pageId,
    .pageType = page.header().pageType,
    .ownerTable = std::move(owner),
    .creationTimestampUnixNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()),
    .status = status,
  };
}

} // namespace zenthrildb
