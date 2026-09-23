#include "core/MetadataManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <stdexcept>

namespace zenthrildb {

MetadataManager::MetadataManager(FileManager& fileManager, FreePageManager& freePageManager)
  : fileManager_(fileManager), freePageManager_(freePageManager) {}

TableMetadata MetadataManager::createTable(const std::string& name) {
  std::lock_guard lock(mutex_);
  if (databaseMetadata_.tableExists(name)) {
    throw std::runtime_error("Table already exists");
  }

  TableMetadata table(nextTableId(), name, freePageManager_.allocatePage());
  databaseMetadata_.upsertTable(table);
  pageDirectory_.registerPage(PageDirectoryEntry{
    .pageId = table.rootPageId(),
    .pageType = PageType::Table,
    .ownerTable = table.name(),
    .creationTimestampUnixNs = table.creationTimestampUnixNs(),
    .status = PageStatus::InUse,
  });
  saveMetadata();
  return table;
}

bool MetadataManager::deleteTable(const std::string& name) {
  std::lock_guard lock(mutex_);
  const auto* table = databaseMetadata_.getTable(name);
  if (table == nullptr) {
    return false;
  }

  freePageManager_.freePage(table->rootPageId());
  pageDirectory_.unregisterPage(table->rootPageId());
  const auto removed = databaseMetadata_.deleteTable(name);
  saveMetadata();
  return removed;
}

bool MetadataManager::renameTable(const std::string& currentName, const std::string& newName) {
  std::lock_guard lock(mutex_);
  const auto* existing = databaseMetadata_.getTable(currentName);
  if (existing == nullptr) {
    return false;
  }
  const auto rootPageId = existing->rootPageId();
  const auto renamed = databaseMetadata_.renameTable(currentName, newName);
  if (!renamed) {
    return false;
  }
  if (const auto* entry = pageDirectory_.get(rootPageId)) {
    auto updatedEntry = *entry;
    updatedEntry.ownerTable = newName;
    pageDirectory_.registerPage(updatedEntry);
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

void MetadataManager::saveMetadata() {
  const auto databasePayload = databaseMetadata_.serialize();
  Page metadataPage;
  metadataPage.header().pageId = kDatabaseMetadataPageId;
  metadataPage.header().pageType = PageType::Metadata;
  auto metadataPayload = metadataPage.payload();
  std::memcpy(metadataPayload.data(), databasePayload.data(), databasePayload.size());
  metadataPage.header().usedBytes = static_cast<std::uint32_t>(databasePayload.size());
  fileManager_.writePage(metadataPage);

  const auto directoryPayload = pageDirectory_.serialize();
  Page directoryPage;
  directoryPage.header().pageId = kPageDirectoryPageId;
  directoryPage.header().pageType = PageType::Metadata;
  auto directoryView = directoryPage.payload();
  std::memcpy(directoryView.data(), directoryPayload.data(), directoryPayload.size());
  directoryPage.header().usedBytes = static_cast<std::uint32_t>(directoryPayload.size());
  fileManager_.writePage(directoryPage);

  freePageManager_.save();
}

void MetadataManager::loadMetadata() {
  std::lock_guard lock(mutex_);

  const auto metadataPage = fileManager_.readPage(kDatabaseMetadataPageId);
  databaseMetadata_.deserialize(std::span<const Byte>(
    metadataPage.payload().data(),
    metadataPage.payload().size()));

  const auto directoryPage = fileManager_.readPage(kPageDirectoryPageId);
  pageDirectory_.deserialize(std::span<const Byte>(
    directoryPage.payload().data(),
    directoryPage.payload().size()));

  freePageManager_.load();
}

std::uint64_t MetadataManager::nextTableId() const noexcept {
  std::uint64_t nextId = 1;
  for (const auto& table : databaseMetadata_.tables()) {
    nextId = std::max(nextId, table.tableId() + 1);
  }
  return nextId;
}

} // namespace zenthrildb
