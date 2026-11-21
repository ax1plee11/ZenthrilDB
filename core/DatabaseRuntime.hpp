#pragma once

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"
#include "core/PageManager.hpp"
#include "core/RecordManager.hpp"
#include "core/StorageConsistencyChecker.hpp"
#include "recovery/CheckpointService.hpp"
#include "storage/SnapshotManifest.hpp"
#include "transaction/StorageMutationCoordinator.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogManager.hpp"

#include <filesystem>
#include <memory>
#include <mutex>

namespace zenthrildb {

// Owns the only supported production manager graph for one opened database.
class DatabaseRuntime final {
public:
  explicit DatabaseRuntime(const std::filesystem::path& path, bool create = false);
  DatabaseRuntime(const DatabaseRuntime&) = delete;
  DatabaseRuntime& operator=(const DatabaseRuntime&) = delete;

  [[nodiscard]] RecordManager& records() noexcept { return *records_; }
  [[nodiscard]] TableMetadata createTable(std::string_view name) { return records_->createTable(name); }
  [[nodiscard]] bool renameTable(std::string_view oldName, std::string_view newName) {
    return records_->renameTable(oldName, newName);
  }
  [[nodiscard]] bool dropTable(std::string_view name) { return records_->dropTable(name); }
  [[nodiscard]] bool tableExists(std::string_view name) const {
    std::lock_guard lock(operationsMutex_);
    return metadata_->tableExists(std::string(name));
  }
  [[nodiscard]] std::vector<std::string> listTables() const {
    std::lock_guard lock(operationsMutex_);
    return metadata_->listTables();
  }
  [[nodiscard]] StorageConsistencyReport checkConsistency();
  [[nodiscard]] CheckpointResult checkpoint();
  void backupTo(const std::filesystem::path& destinationDirectory);
  static void snapshotTo(const std::filesystem::path& source, const std::filesystem::path& destinationDirectory,
                         bool requireBaseline = false);
  static void reduceWalTo(const std::filesystem::path& source, const std::filesystem::path& destinationDirectory);
  static void installReduction(const std::filesystem::path& source, const std::filesystem::path& candidate);
  [[nodiscard]] static SnapshotManifest verifySnapshot(const std::filesystem::path& database,
                                                        bool baselineOnly = false);

private:
  static void createSnapshot(const std::filesystem::path& source, const std::filesystem::path& destinationDirectory,
                             bool requireBaseline, bool reduceHistory);
  std::filesystem::path path_;
  FileManager files_;
  mutable std::recursive_mutex operationsMutex_;
  std::unique_ptr<LogManager> wal_;
  std::unique_ptr<TransactionManager> transactions_;
  std::unique_ptr<StorageMutationCoordinator> mutations_;
  std::unique_ptr<FreePageManager> freePages_;
  std::unique_ptr<MetadataManager> metadata_;
  std::unique_ptr<PageManager> pages_;
  std::unique_ptr<RecordManager> records_;
};

} // namespace zenthrildb
