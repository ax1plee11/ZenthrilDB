#pragma once

#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "storage/DatabaseMetadata.hpp"
#include "storage/PageDirectory.hpp"
#include "storage/TableMetadata.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zenthrildb {

class MetadataManager {
public:
  MetadataManager(FileManager& fileManager, FreePageManager& freePageManager);

  TableMetadata createTable(const std::string& name);
  bool deleteTable(const std::string& name);
  bool renameTable(const std::string& currentName, const std::string& newName);
  void updateMetadata(const TableMetadata& table);
  [[nodiscard]] std::optional<TableMetadata> getTable(const std::string& name) const;
  [[nodiscard]] bool tableExists(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> listTables() const;

  void saveMetadata();
  void loadMetadata();

  [[nodiscard]] const DatabaseMetadata& databaseMetadata() const noexcept { return databaseMetadata_; }
  [[nodiscard]] const PageDirectory& pageDirectory() const noexcept { return pageDirectory_; }

private:
  [[nodiscard]] std::uint64_t nextTableId() const noexcept;

  FileManager& fileManager_;
  FreePageManager& freePageManager_;
  mutable std::mutex mutex_;
  DatabaseMetadata databaseMetadata_{};
  PageDirectory pageDirectory_{};
};

} // namespace zenthrildb
