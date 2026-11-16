#pragma once

#include <cstdint>

namespace zenthrildb {

class FileManager;
class FreePageManager;
class LogManager;
class MetadataManager;

struct StorageConsistencyReport {
  std::uint64_t pagesChecked{0};
  std::uint64_t walRecordsChecked{0};
  std::uint64_t transactionsChecked{0};
};

class StorageConsistencyChecker final {
public:
  [[nodiscard]] StorageConsistencyReport check(
    FileManager& files, FreePageManager& freePages,
    MetadataManager& metadata, LogManager& wal) const;
  void checkPages(FileManager& files, FreePageManager& freePages, MetadataManager& metadata,
                  std::uint64_t lastLsn) const;
};

} // namespace zenthrildb
