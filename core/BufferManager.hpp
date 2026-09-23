#pragma once

#include "core/FileManager.hpp"

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

namespace zenthrildb {

class BufferPageGuard;

class BufferManager {
public:
  explicit BufferManager(FileManager& fileManager, std::size_t capacityPages);

  [[nodiscard]] Page& pin(PageId pageId);
  [[nodiscard]] BufferPageGuard pinGuard(PageId pageId);
  void markDirty(PageId pageId);
  void unpin(PageId pageId);
  void flushAll();

private:
  struct CacheEntry {
    Page page;
    bool dirty{false};
    std::size_t pinCount{0};
    std::list<PageId>::iterator lruPosition;
  };

  void evictIfNeeded();

  FileManager& fileManager_;
  std::size_t capacityPages_;
  mutable std::mutex mutex_;
  std::list<PageId> lru_;
  std::unordered_map<PageId, CacheEntry> cache_;
};

class BufferPageGuard {
public:
  BufferPageGuard() = default;
  BufferPageGuard(BufferManager& bufferManager, PageId pageId, Page& page) noexcept;
  ~BufferPageGuard();

  BufferPageGuard(const BufferPageGuard&) = delete;
  BufferPageGuard& operator=(const BufferPageGuard&) = delete;

  BufferPageGuard(BufferPageGuard&& other) noexcept;
  BufferPageGuard& operator=(BufferPageGuard&& other) noexcept;

  [[nodiscard]] Page& page();
  [[nodiscard]] const Page& page() const;
  [[nodiscard]] PageId pageId() const noexcept { return pageId_; }
  [[nodiscard]] bool valid() const noexcept { return bufferManager_ != nullptr && page_ != nullptr; }

  void markDirty();
  void release() noexcept;

private:
  BufferManager* bufferManager_{nullptr};
  PageId pageId_{0};
  Page* page_{nullptr};
};

} // namespace zenthrildb
