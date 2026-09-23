#pragma once

#include "storage/PageHeader.hpp"
#include "storage/TableMetadata.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace zenthrildb {

class DatabaseMetadata {
public:
  static constexpr std::uint32_t kMetadataVersion = 1;
  static constexpr std::size_t kStorageSize = kDefaultPageSize - PageHeader::serializedSize();

  [[nodiscard]] const std::vector<TableMetadata>& tables() const noexcept { return tables_; }
  [[nodiscard]] bool tableExists(const std::string& name) const;
  [[nodiscard]] const TableMetadata* getTable(const std::string& name) const;
  void upsertTable(const TableMetadata& table);
  bool renameTable(const std::string& currentName, const std::string& newName);
  bool deleteTable(const std::string& name);
  [[nodiscard]] std::vector<std::string> listTables() const;

  [[nodiscard]] std::array<Byte, kStorageSize> serialize() const;
  void deserialize(std::span<const Byte> bytes);

private:
  std::uint32_t metadataVersion_{kMetadataVersion};
  std::uint64_t nextTableId_{1};
  std::vector<TableMetadata> tables_{};
};

} // namespace zenthrildb
