#include "core/MetadataManager.hpp"
#include "storage/BinaryFormat.hpp"
#include "storage/BinaryIO.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {

MetadataManager::MetadataManager(FileManager& fileManager, FreePageManager& freePageManager,
                                 StorageMutationCoordinator& mutationCoordinator)
  : fileManager_(fileManager), freePageManager_(freePageManager), mutationCoordinator_(&mutationCoordinator) {}

TableMetadata MetadataManager::createTable(const std::string& name) {
  std::lock_guard lock(mutex_);
  if (databaseMetadata_.tableExists(name)) {
    throw std::runtime_error("Table already exists");
  }
  constexpr auto maxTables =
    (DatabaseMetadata::kStorageSize - format::kDatabaseMetadataEntriesOffset) / TableMetadata::kSerializedSize;
  if (databaseMetadata_.tables().size() >= maxTables) {
    throw std::length_error("Database table catalog is full");
  }
  TableMetadata table(nextTableId(), name, 0);

  const auto beforeHeader = fileManager_.header();
  const auto rootPageId = freePageManager_.allocatePageDeferred();
  const auto resultingPageCount = std::max<std::uint64_t>(beforeHeader.totalPages,
    static_cast<std::uint64_t>(rootPageId) + 1);
  Page rootPage;
  rootPage.header().pageId = rootPageId;
  rootPage.header().pageType = PageType::Table;

  table.setRootPageId(rootPageId);
  databaseMetadata_.upsertTable(table);
  pageDirectory_.registerPage(PageDirectoryEntry{
    .pageId = table.rootPageId(),
    .pageType = PageType::Table,
    .ownerTable = table.name(),
    .creationTimestampUnixNs = table.creationTimestampUnixNs(),
    .status = PageStatus::InUse,
  });
  saveMetadata({rootPage}, beforeHeader, resultingPageCount);
  freePageManager_.completeDeferredAllocation(rootPageId);
  return table;
}

bool MetadataManager::deleteTable(const std::string& name) {
  std::lock_guard lock(mutex_);
  const auto* table = databaseMetadata_.getTable(name);
  if (table == nullptr) {
    return false;
  }

  const auto rootPageId = table->rootPageId();
  const auto directoryEntries = pageDirectory_.entries();
  const auto ownsOverflow = [&](const PageDirectoryEntry& entry) {
    return entry.ownerTable == name && entry.status == PageStatus::InUse && entry.pageType == PageType::Overflow;
  };
  freePageManager_.checkFreeListCapacity(1 + static_cast<std::size_t>(std::count_if(
    directoryEntries.begin(), directoryEntries.end(), ownsOverflow)));
  std::vector<Page> freedPages;
  Page freeRoot;
  freeRoot.header().pageId = rootPageId;
  freeRoot.header().pageType = PageType::Free;
  freePageManager_.freePageDeferred(rootPageId);
  pageDirectory_.registerPage(PageDirectoryEntry{
    .pageId = rootPageId,
    .pageType = PageType::Free,
    .ownerTable = {},
    .creationTimestampUnixNs = 0,
    .status = PageStatus::Free,
  });
  freedPages.push_back(freeRoot);

  for (const auto& entry : directoryEntries) {
    if (!ownsOverflow(entry)) continue;
    freePageManager_.freePageDeferred(entry.pageId);
    Page freePage;
    freePage.header().pageId = entry.pageId;
    freePage.header().pageType = PageType::Free;
    freedPages.push_back(freePage);
    pageDirectory_.registerPage(PageDirectoryEntry{
      .pageId = entry.pageId,
      .pageType = PageType::Free,
      .ownerTable = {},
      .creationTimestampUnixNs = 0,
      .status = PageStatus::Free,
    });
  }
  const auto removed = databaseMetadata_.deleteTable(name);
  saveMetadata(std::move(freedPages));
  return removed;
}

bool MetadataManager::renameTable(const std::string& currentName, const std::string& newName) {
  std::lock_guard lock(mutex_);
  const auto* existing = databaseMetadata_.getTable(currentName);
  if (existing == nullptr) {
    return false;
  }
  const auto renamed = databaseMetadata_.renameTable(currentName, newName);
  if (!renamed) {
    return false;
  }
  for (auto entry : pageDirectory_.entries()) {
    if (entry.ownerTable == currentName) {
      entry.ownerTable = newName;
      pageDirectory_.registerPage(entry);
    }
  }
  saveMetadata();
  return true;
}

void MetadataManager::updateMetadata(const TableMetadata& table) {
  std::lock_guard lock(mutex_);
  if (!databaseMetadata_.tableExists(table.name())) {
    throw std::runtime_error("Cannot update missing table metadata");
  }
  databaseMetadata_.upsertTable(table);
  if (const auto* entry = pageDirectory_.get(table.rootPageId())) {
    auto updatedEntry = *entry;
    updatedEntry.ownerTable = table.name();
    updatedEntry.creationTimestampUnixNs = table.creationTimestampUnixNs();
    pageDirectory_.registerPage(updatedEntry);
  }
  saveMetadata();
}

void MetadataManager::registerPage(const PageDirectoryEntry& entry, std::vector<Page> additionalPages,
                                   std::optional<DatabaseHeader> beforeHeader,
                                   std::optional<std::uint64_t> resultingPageCount) {
  registerPages({entry}, std::move(additionalPages), beforeHeader, resultingPageCount);
}

void MetadataManager::registerPages(std::vector<PageDirectoryEntry> entries, std::vector<Page> additionalPages,
                                    std::optional<DatabaseHeader> beforeHeader,
                                    std::optional<std::uint64_t> resultingPageCount) {
  std::lock_guard lock(mutex_);
  std::unordered_set<PageId> uniqueIds;
  for (const auto& entry : entries) {
    if (entry.pageId < kFirstUserPageId || isDirectoryContinuation(entry.pageId) || entry.status == PageStatus::Deleted ||
        (entry.status == PageStatus::Free && entry.pageType != PageType::Free) ||
        (entry.status == PageStatus::InUse && entry.pageType == PageType::Free) ||
        !uniqueIds.insert(entry.pageId).second) {
      throw std::invalid_argument("Invalid or duplicate user page directory entry");
    }
  }
  std::vector<std::pair<PageId, std::optional<PageDirectoryEntry>>> previous;
  previous.reserve(entries.size());
  for (const auto& entry : entries) {
    const auto* oldEntry = pageDirectory_.get(entry.pageId);
    previous.emplace_back(entry.pageId, oldEntry ? std::optional<PageDirectoryEntry>(*oldEntry) : std::nullopt);
    pageDirectory_.registerPage(entry);
  }
  try {
    persistDirectory(std::move(additionalPages), beforeHeader, resultingPageCount);
  } catch (...) {
    for (auto& [pageId, oldEntry] : previous) {
      if (oldEntry) pageDirectory_.registerPage(*oldEntry);
      else pageDirectory_.unregisterPage(pageId);
    }
    throw;
  }
}

void MetadataManager::unregisterPage(PageId pageId, std::vector<Page> additionalPages) {
  std::lock_guard lock(mutex_);
  const auto previous = pageDirectory_.get(pageId)
    ? std::optional<PageDirectoryEntry>(*pageDirectory_.get(pageId)) : std::nullopt;
  pageDirectory_.unregisterPage(pageId);
  try {
    persistDirectory(std::move(additionalPages), std::nullopt, std::nullopt);
  } catch (...) {
    if (previous) pageDirectory_.registerPage(*previous);
    throw;
  }
}

std::optional<TableMetadata> MetadataManager::getTable(const std::string& name) const {
  std::lock_guard lock(mutex_);
  if (const auto* table = databaseMetadata_.getTable(name)) {
    return *table;
  }
  return std::nullopt;
}

bool MetadataManager::tableExists(const std::string& name) const {
  std::lock_guard lock(mutex_);
  return databaseMetadata_.tableExists(name);
}

std::vector<std::string> MetadataManager::listTables() const {
  std::lock_guard lock(mutex_);
  return databaseMetadata_.listTables();
}

void MetadataManager::saveMetadata(std::vector<Page> additionalPages,
                                   std::optional<DatabaseHeader> beforeHeader,
                                   std::optional<std::uint64_t> resultingPageCount) {
  const auto databasePayload = databaseMetadata_.serialize();
  Page metadataPage;
  metadataPage.header().pageId = kDatabaseMetadataPageId;
  metadataPage.header().pageType = PageType::Metadata;
  metadataPage.header().version = Page::kFormatVersionV2;
  auto metadataPayload = metadataPage.payload();
  if (databasePayload.size() > metadataPayload.size()) throw std::runtime_error("Database metadata exceeds v2 page capacity");
  std::memcpy(metadataPayload.data(), databasePayload.data(), databasePayload.size());
  metadataPage.header().usedBytes = static_cast<std::uint32_t>(databasePayload.size());

  additionalPages.push_back(metadataPage);
  persistDirectory(std::move(additionalPages), beforeHeader, resultingPageCount);
}

void MetadataManager::persistDirectory(std::vector<Page> pages, std::optional<DatabaseHeader> beforeHeader,
                                       std::optional<std::uint64_t> resultingPageCount) {
  try {
    for (const auto& page : pages) {
      if (isDirectoryContinuation(page.header().pageId)) {
        throw std::invalid_argument("Storage mutation cannot overwrite a page directory continuation");
      }
    }
    auto continuations = directoryContinuations_;
    if (continuations.empty() && pageDirectory_.size() <= PageDirectory::kSinglePageCapacity) {
      const auto payload = pageDirectory_.serialize();
      Page page;
      page.header().pageId = kPageDirectoryPageId;
      page.header().pageType = PageType::Metadata;
      page.header().version = Page::kFormatVersionV2;
      std::copy_n(payload.begin(), page.payload().size(), page.payload().begin());
      page.header().usedBytes = static_cast<std::uint32_t>(page.payload().size());
      pages.push_back(std::move(page));
    } else {
      std::vector<PageId> chunkIds(continuations.begin(), continuations.end());
      std::sort(chunkIds.begin(), chunkIds.end());
      chunkIds.insert(chunkIds.begin(), kPageDirectoryPageId);
      const auto currentHeader = fileManager_.header();
      auto nextPageId = resultingPageCount.value_or(currentHeader.totalPages);
      const auto entries = pageDirectory_.entries();
      constexpr auto capacity = PageDirectory::kChunkCapacity;
      const auto required = entries.size() / capacity + (entries.size() % capacity != 0);
      while (chunkIds.size() < required) {
        if (nextPageId > std::numeric_limits<PageId>::max()) {
          throw std::overflow_error("Page identifier space exhausted while extending the directory");
        }
        const auto id = static_cast<PageId>(nextPageId++);
        chunkIds.push_back(id);
        continuations.insert(id);
      }
      if (nextPageId != resultingPageCount.value_or(currentHeader.totalPages)) {
        if (!beforeHeader) beforeHeader = currentHeader;
        resultingPageCount = nextPageId;
      }
      std::span remaining(entries);
      for (std::size_t index = 0; index < chunkIds.size(); ++index) {
        const auto id = chunkIds[index];
        const auto next = index + 1 < chunkIds.size() ? chunkIds[index + 1] : PageId{0};
        const auto count = std::min(remaining.size(), capacity);
        auto chunk = PageDirectory::serializeChunk(id, next, remaining.first(count));
        remaining = remaining.subspan(count);
        if (id < currentHeader.totalPages) {
          const auto existing = fileManager_.readPage(id);
          if (existing.header().pageType == PageType::Metadata && existing.header().version == chunk.header().version &&
              existing.header().usedBytes == chunk.header().usedBytes &&
              std::ranges::equal(existing.payload(), chunk.payload())) continue;
        }
        pages.push_back(std::move(chunk));
      }
    }
    pages.push_back(freePageManager_.snapshotFreeListPage());
    if (!mutationCoordinator_) throw std::logic_error("Read-only metadata manager");
    mutationCoordinator_->writePagesAtomically(std::move(pages), beforeHeader, resultingPageCount);
    directoryContinuations_.swap(continuations);
  } catch (...) {
    fileManager_.markFailed();
    throw;
  }
}

void MetadataManager::loadMetadata() {
  std::lock_guard lock(mutex_);

  const auto metadataPage = fileManager_.readPage(kDatabaseMetadataPageId);
  databaseMetadata_.deserialize(std::span<const Byte>(
    metadataPage.payload().data(),
    metadataPage.payload().size()));

  const auto root = fileManager_.readPage(kPageDirectoryPageId);
  if (root.header().pageId != kPageDirectoryPageId || root.header().pageType != PageType::Metadata) {
    throw std::invalid_argument("Invalid page directory root");
  }
  PageDirectory loadedDirectory;
  std::unordered_set<PageId> continuations;
  std::size_t offset = 0;
  if (binary::readLittleEndian<std::uint32_t>(root.payload(), offset) == PageDirectory::kChainedVersion) {
    auto next = loadedDirectory.deserializeChunk(root);
    auto previous = kPageDirectoryPageId;
    while (next != 0) {
      if (next < kFirstUserPageId || next <= previous || next >= fileManager_.header().totalPages) {
        throw std::invalid_argument("Invalid page directory continuation id");
      }
      continuations.insert(next);
      const auto chunk = fileManager_.readPage(next);
      if (chunk.header().pageId != next) throw std::invalid_argument("Invalid page directory continuation page");
      previous = next;
      next = loadedDirectory.deserializeChunk(chunk);
    }
  } else {
    loadedDirectory.deserialize(root.payload());
  }
  freePageManager_.load();
  for (const auto id : continuations) {
    if (loadedDirectory.get(id) != nullptr || freePageManager_.freePages().contains(id)) {
      throw std::invalid_argument("Page directory continuation is marked as a user or free page");
    }
  }
  pageDirectory_ = std::move(loadedDirectory);
  directoryContinuations_.swap(continuations);
}

std::uint64_t MetadataManager::nextTableId() const noexcept {
  std::uint64_t nextId = 1;
  for (const auto& table : databaseMetadata_.tables()) {
    nextId = std::max(nextId, table.tableId() + 1);
  }
  return nextId;
}

} // namespace zenthrildb
