#include "storage/DatabaseMetadata.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/BinaryFormat.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {

bool DatabaseMetadata::tableExists(const std::string& name) const {
  return getTable(name) != nullptr;
}

const TableMetadata* DatabaseMetadata::getTable(const std::string& name) const {
  for (const auto& table : tables_) {
    if (table.name() == name) {
      return &table;
    }
  }
  return nullptr;
}

void DatabaseMetadata::upsertTable(const TableMetadata& table) {
  for (auto& existing : tables_) {
    if (existing.name() == table.name()) {
      existing = table;
      return;
    }
  }
  tables_.push_back(table);
}

bool DatabaseMetadata::renameTable(const std::string& currentName, const std::string& newName) {
  if (tableExists(newName)) {
    throw std::runtime_error("Target table name already exists");
  }
  for (auto& table : tables_) {
    if (table.name() == currentName) {
      table.rename(newName);
      return true;
    }
  }
  return false;
}

bool DatabaseMetadata::deleteTable(const std::string& name) {
  const auto oldSize = tables_.size();
  tables_.erase(std::remove_if(tables_.begin(), tables_.end(),
                               [&](const auto& t) { return t.name() == name; }),
                tables_.end());
  return tables_.size() != oldSize;
}

std::vector<std::string> DatabaseMetadata::listTables() const {
  std::vector<std::string> names;
  names.reserve(tables_.size());
  for (const auto& table : tables_) {
    names.push_back(table.name());
  }
  return names;
}

std::array<Byte, DatabaseMetadata::kStorageSize> DatabaseMetadata::serialize() const {
  std::array<Byte, DatabaseMetadata::kStorageSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, format::kDatabaseMetadataFormatVersion);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, nextTableId_);
  const auto count = static_cast<std::uint32_t>(tables_.size());
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, count);
  for (const auto& table : tables_) {
    const auto tableBytes = table.serialize();
    if (offset + tableBytes.size() > bytes.size()) {
      throw std::runtime_error("Database metadata exceeds page size");
    }
    binary::writeBytes(bytes, offset, tableBytes);
  }
  return bytes;
}

void DatabaseMetadata::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != kStorageSize) {
    throw std::invalid_argument("Invalid database metadata page size");
  }
  std::size_t offset = 0;
  metadataVersion_ = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (metadataVersion_ != format::kDatabaseMetadataFormatVersion) {
    throw std::invalid_argument("Unsupported database metadata format version");
  }
  nextTableId_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  const auto count = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (count > (bytes.size() - format::kDatabaseMetadataEntriesOffset) / TableMetadata::kSerializedSize) {
    throw std::invalid_argument("Database metadata table count exceeds payload capacity");
  }
  tables_.clear();
  std::unordered_set<std::string> names;
  std::unordered_set<std::uint64_t> ids;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (offset + TableMetadata::kSerializedSize > bytes.size()) {
      throw std::invalid_argument("Database metadata table list is truncated");
    }
    TableMetadata table;
    table.deserialize(std::span<const Byte>(bytes.data() + offset, TableMetadata::kSerializedSize));
    offset += TableMetadata::kSerializedSize;
    if (!names.insert(table.name()).second || !ids.insert(table.tableId()).second) {
      throw std::invalid_argument("Duplicate table metadata detected");
    }
    tables_.push_back(table);
  }
}

} // namespace zenthrildb
