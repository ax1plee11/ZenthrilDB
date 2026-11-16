#pragma once

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "storage/DatabaseMetadata.hpp"
#include "storage/PageDirectory.hpp"
#include "storage/TableMetadata.hpp"
#include "transaction/StorageMutationCoordinator.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace zenthrildb {

class MetadataManager {
public:
  MetadataManager(FileManager& fileManager, FreePageManager& freePageManager,
                  StorageMutationCoordinator& mutationCoordinator);

  TableMetadata createTable(const std::string& name);
  bool deleteTable(const std::string& name);
  bool renameTable(const std::string& currentName, const std::string& newName);
  void updateMetadata(const TableMetadata& table);
  void registerPage(const PageDirectoryEntry& entry, std::vector<Page> additionalPages,
                    std::optional<DatabaseHeader> beforeHeader = std::nullopt,
                    std::optional<std::uint64_t> resultingPageCount = std::nullopt);
  void registerPages(std::vector<PageDirectoryEntry> entries, std::vector<Page> additionalPages,
                     std::optional<DatabaseHeader> beforeHeader = std::nullopt,
                     std::optional<std::uint64_t> resultingPageCount = std::nullopt);
  void unregisterPage(PageId pageId, std::vector<Page> additionalPages);
  [[nodiscard]] std::optional<TableMetadata> getTable(const std::string& name) const;
  [[nodiscard]] bool tableExists(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> listTables() const;

  void saveMetadata(std::vector<Page> additionalPages = {},
                    std::optional<DatabaseHeader> beforeHeader = std::nullopt,
                    std::optional<std::uint64_t> resultingPageCount = std::nullopt);
  void loadMetadata();

  [[nodiscard]] const DatabaseMetadata& databaseMetadata() const noexcept { return databaseMetadata_; }
  [[nodiscard]] const PageDirectory& pageDirectory() const noexcept { return pageDirectory_; }
  [[nodiscard]] bool isDirectoryContinuation(PageId id) const noexcept { return directoryContinuations_.contains(id); }

private:
  friend class SnapshotBaseline;
  MetadataManager(FileManager& fileManager, FreePageManager& freePageManager)
    : fileManager_(fileManager), freePageManager_(freePageManager) {}
  void persistDirectory(std::vector<Page> pages, std::optional<DatabaseHeader> beforeHeader,
                        std::optional<std::uint64_t> resultingPageCount);
  [[nodiscard]] std::uint64_t nextTableId() const noexcept;

  FileManager& fileManager_;
  FreePageManager& freePageManager_;
  StorageMutationCoordinator* mutationCoordinator_{};
  mutable std::mutex mutex_;
  DatabaseMetadata databaseMetadata_{};
  PageDirectory pageDirectory_{};
  std::unordered_set<PageId> directoryContinuations_;
};

} // namespace zenthrildb
