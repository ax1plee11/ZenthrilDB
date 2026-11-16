#include "core/PageManager.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace zenthrildb {

PageManager::PageManager(FileManager& fileManager, FreePageManager& freePageManager, MetadataManager& metadataManager)
  : fileManager_(fileManager), freePageManager_(freePageManager), metadataManager_(metadataManager) {}

Page PageManager::allocatePage(PageType type) {
  return allocatePage(type, {});
}

Page PageManager::allocatePage(PageType type, std::string owner) {
  std::lock_guard lock(mutex_);
  if (type != PageType::Table && type != PageType::Index && type != PageType::Overflow) {
    throw std::invalid_argument("PageManager can allocate only user data page types");
  }
  const auto beforeHeader = fileManager_.header();
  Page page;
  page.header().pageId = freePageManager_.allocatePageDeferred();
  page.header().pageType = type;
  page.header().usedBytes = 0;
  const auto resultingPageCount = std::max<std::uint64_t>(beforeHeader.totalPages,
    static_cast<std::uint64_t>(page.header().pageId) + 1);
  metadataManager_.registerPage(makeDirectoryEntry(page, owner, PageStatus::InUse),
    {page}, beforeHeader, resultingPageCount);
  freePageManager_.completeDeferredAllocation(page.header().pageId);
  dirtyPages_.insert(page.header().pageId);
  return page;
}

void PageManager::freePage(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (pageId < kFirstUserPageId) {
    throw std::runtime_error("Reserved pages cannot be freed");
  }
  const auto* entry = metadataManager_.pageDirectory().get(pageId);
  if (entry == nullptr || entry->status != PageStatus::InUse) {
    throw std::invalid_argument("Only an allocated user page can be freed");
  }
  if (fileManager_.readPage(pageId).header().pageType == PageType::Free) {
    throw std::invalid_argument("Page is already free");
  }
  Page freePage;
  freePage.header().pageId = pageId;
  freePage.header().pageType = PageType::Free;
  freePage.header().usedBytes = 0;
  freePageManager_.freePageDeferred(pageId);
  metadataManager_.registerPage(makeDirectoryEntry(freePage, {}, PageStatus::Free), {freePage});
  dirtyPages_.erase(pageId);
}

void PageManager::allocatePageAndWrite(Page page, std::string owner, Page updatedPage,
                                      const std::function<void(Page&, PageId)>& linkPage) {
  std::lock_guard lock(mutex_);
  const auto* entry = metadataManager_.pageDirectory().get(updatedPage.header().pageId);
  if (entry == nullptr || entry->status != PageStatus::InUse || entry->pageType != updatedPage.header().pageType) {
    throw std::invalid_argument("Page link must belong to an allocated page");
  }

  const auto beforeHeader = fileManager_.header();
  const auto pageId = freePageManager_.allocatePageDeferred();
  try {
    page.header().pageId = pageId;
    linkPage(updatedPage, pageId);
    const auto resultingPageCount = std::max(beforeHeader.totalPages, std::uint64_t{pageId} + 1);
    auto newEntry = makeDirectoryEntry(page, std::move(owner), PageStatus::InUse);
    metadataManager_.registerPage(newEntry, {page, updatedPage}, beforeHeader, resultingPageCount);
    freePageManager_.completeDeferredAllocation(pageId);
  } catch (...) {
    fileManager_.markFailed();
    throw;
  }
}

void PageManager::freePageAndWrite(PageId pageId, const Page& updatedPage) {
  std::lock_guard lock(mutex_);
  if (pageId < kFirstUserPageId || updatedPage.header().pageId == pageId) {
    throw std::invalid_argument("Invalid page ids for atomic unlink/free mutation");
  }
  const auto* freeEntry = metadataManager_.pageDirectory().get(pageId);
  const auto* updatedEntry = metadataManager_.pageDirectory().get(updatedPage.header().pageId);
  if (freeEntry == nullptr || freeEntry->status != PageStatus::InUse ||
      updatedEntry == nullptr || updatedEntry->status != PageStatus::InUse ||
      updatedPage.header().pageType != updatedEntry->pageType) {
    throw std::invalid_argument("Atomic unlink/free requires two allocated pages");
  }
  Page freePage;
  freePage.header().pageId = pageId;
  freePage.header().pageType = PageType::Free;
  auto freeDirectoryEntry = makeDirectoryEntry(freePage, {}, PageStatus::Free);
  auto updatedDirectoryEntry = *updatedEntry;
  std::vector<Page> pages{updatedPage, freePage};
  std::vector<PageDirectoryEntry> entries{std::move(freeDirectoryEntry), std::move(updatedDirectoryEntry)};
  freePageManager_.checkFreeListCapacity(1);
  try {
    freePageManager_.freePageDeferred(pageId);
    metadataManager_.registerPages(std::move(entries), std::move(pages));
  } catch (...) {
    fileManager_.markFailed();
    throw;
  }
  dirtyPages_.erase(pageId);
  dirtyPages_.insert(updatedPage.header().pageId);
}

Page PageManager::readPage(PageId pageId) {
  std::lock_guard lock(mutex_);
  auto page = fileManager_.readPage(pageId);
  return page;
}

std::uint64_t PageManager::databasePageCount() const {
  std::lock_guard lock(mutex_);
  return fileManager_.header().totalPages;
}

void PageManager::writePage(const Page& page) {
  std::lock_guard lock(mutex_);
  if (page.header().pageId < kFirstUserPageId) {
    throw std::runtime_error("Reserved pages must be managed by their owning subsystem");
  }
  const auto* existing = metadataManager_.pageDirectory().get(page.header().pageId);
  if (existing == nullptr || existing->status != PageStatus::InUse || page.header().pageType == PageType::Free ||
      (page.header().pageType != PageType::Table && page.header().pageType != PageType::Index &&
       page.header().pageType != PageType::Overflow)) {
    throw std::invalid_argument("Page must be allocated and have a user data page type before writing");
  }
  const auto owner = existing->ownerTable;
  const auto timestamp = existing->creationTimestampUnixNs;
  auto entry = makeDirectoryEntry(page, owner, PageStatus::InUse);
  entry.creationTimestampUnixNs = timestamp;
  metadataManager_.registerPage(entry, {page});
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
  if (const auto* entry = metadataManager_.pageDirectory().get(pageId)) {
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
