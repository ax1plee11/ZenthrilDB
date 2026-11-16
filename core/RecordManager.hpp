#pragma once

#include "core/PageManager.hpp"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zenthrildb {

class MetadataManager;

// Durable heap records stored as a linked list of checksummed pages.
class RecordManager final {
public:
  RecordManager(PageManager& pages, MetadataManager& metadata, std::recursive_mutex& runtimeMutex);

  [[nodiscard]] TableMetadata createTable(std::string_view table);
  [[nodiscard]] bool renameTable(std::string_view currentName, std::string_view newName);
  [[nodiscard]] bool dropTable(std::string_view table);
  void put(std::string_view table, std::string_view key, std::span<const Byte> value);
  [[nodiscard]] std::optional<std::vector<Byte>> get(std::string_view table, std::string_view key);
  [[nodiscard]] bool erase(std::string_view table, std::string_view key);
  [[nodiscard]] std::vector<std::pair<std::string, std::vector<Byte>>> scan(std::string_view table);
  void validate();

private:
  struct LocatedRecord { Page page; std::uint32_t valueOffset; std::uint32_t valueSize; };
  [[nodiscard]] std::optional<LocatedRecord> find(const TableMetadata& table, std::string_view key);
  [[nodiscard]] std::vector<Page> readChain(const TableMetadata& table);
  [[nodiscard]] TableMetadata requireTable(std::string_view table) const;
  void initializeRoot(Page& root) const;

  PageManager& pages_;
  MetadataManager& metadata_;
  std::recursive_mutex& mutex_;
};

} // namespace zenthrildb
