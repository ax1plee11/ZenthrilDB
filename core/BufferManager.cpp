#include "core/BufferManager.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace zenthrildb {

BufferManager::BufferManager(FileManager& fileManager, std::size_t capacityPages)
  : fileManager_(fileManager), capacityPages_(capacityPages) {
  if (capacityPages_ == 0) {
    throw std::invalid_argument("Buffer capacity must be greater than zero");
  }
}

Page& BufferManager::pin(PageId pageId) {
  std::lock_guard lock(mutex_);
  if (auto it = cache_.find(pageId); it != cache_.end()) {
    ++it->second.pinCount;
    lru_.splice(lru_.begin(), lru_, it->second.lruPosition);
    return it->second.page;
  }

  evictIfNeeded();
  auto page = fileManager_.readPage(pageId);
  lru_.push_front(pageId);
  auto lruPosition = lru_.begin();
  auto [it, inserted] = cache_.emplace(pageId, CacheEntry{std::move(page), false, 1, lruPosition});
  if (!inserted) {
    throw std::runtime_error("Unexpected buffer cache collision");
  }
  return it->second.page;
}

BufferPageGuard BufferManager::pinGuard(PageId pageId) {
  auto& page = pin(pageId);
  return BufferPageGuard(*this, pageId, page);
}

void BufferManager::markDirty(PageId pageId) {
  std::lock_guard lock(mutex_);
  auto it = cache_.find(pageId);
  if (it == cache_.end()) {
    throw std::runtime_error("Cannot mark missing page dirty");
  }
  it->second.dirty = true;
}

void BufferManager::unpin(PageId pageId) {
  std::lock_guard lock(mutex_);
  auto it = cache_.find(pageId);
  if (it == cache_.end()) {
    return;
  }
  if (it->second.pinCount > 0) {
    --it->second.pinCount;
  }
}

void BufferManager::flushAll() {
  std::lock_guard lock(mutex_);
  for (auto& [pageId, entry] : cache_) {
    if (entry.dirty) {
      fileManager_.writePage(entry.page);
      entry.dirty = false;
    }
  }
  fileManager_.flush();
}

void BufferManager::evictIfNeeded() {
  if (cache_.size() < capacityPages_) {
    return;
  }

  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    auto cacheIt = cache_.find(*it);
    if (cacheIt != cache_.end() && cacheIt->second.pinCount == 0) {
      if (cacheIt->second.dirty) {
        fileManager_.writePage(cacheIt->second.page);
      }
      lru_.erase(cacheIt->second.lruPosition);
      cache_.erase(cacheIt);
      return;
    }
  }

  throw std::runtime_error("No evictable pages in buffer");
}

BufferPageGuard::BufferPageGuard(BufferManager& bufferManager, PageId pageId, Page& page) noexcept
  : bufferManager_(&bufferManager), pageId_(pageId), page_(&page) {}

BufferPageGuard::~BufferPageGuard() {
  release();
}

BufferPageGuard::BufferPageGuard(BufferPageGuard&& other) noexcept
  : bufferManager_(other.bufferManager_), pageId_(other.pageId_), page_(other.page_) {
  other.bufferManager_ = nullptr;
  other.page_ = nullptr;
  other.pageId_ = 0;
}

BufferPageGuard& BufferPageGuard::operator=(BufferPageGuard&& other) noexcept {
  if (this != &other) {
    release();
    bufferManager_ = other.bufferManager_;
    pageId_ = other.pageId_;
    page_ = other.page_;
    other.bufferManager_ = nullptr;
    other.page_ = nullptr;
    other.pageId_ = 0;
  }
  return *this;
}

Page& BufferPageGuard::page() {
  if (!valid()) {
    throw std::runtime_error("Invalid buffer page guard");
  }
  return *page_;
}

const Page& BufferPageGuard::page() const {
  if (!valid()) {
    throw std::runtime_error("Invalid buffer page guard");
  }
  return *page_;
}

void BufferPageGuard::markDirty() {
  if (!valid()) {
    throw std::runtime_error("Invalid buffer page guard");
  }
  bufferManager_->markDirty(pageId_);
}

void BufferPageGuard::release() noexcept {
  if (bufferManager_ != nullptr) {
    bufferManager_->unpin(pageId_);
    bufferManager_ = nullptr;
    page_ = nullptr;
    pageId_ = 0;
  }
}

} // namespace zenthrildb
