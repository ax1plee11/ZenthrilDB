#pragma once

#include "storage/Page.hpp"
#include "storage/DatabaseHeader.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace zenthrildb {

class FileManager;
class LogManager;
class TransactionManager;

// Applies one physical mutation as page/header images under the database's existing WAL/TM.
class StorageMutationCoordinator final {
public:
  StorageMutationCoordinator(FileManager& files, LogManager& wal, TransactionManager& transactions);
  ~StorageMutationCoordinator();
  StorageMutationCoordinator(const StorageMutationCoordinator&) = delete;
  StorageMutationCoordinator& operator=(const StorageMutationCoordinator&) = delete;

  void writePagesAtomically(std::vector<Page> pages,
                            std::optional<DatabaseHeader> beforeHeader = std::nullopt,
                            std::optional<std::uint64_t> resultingPageCount = std::nullopt);

private:
  FileManager& files_;
  LogManager& wal_;
  TransactionManager& transactions_;
  std::mutex mutex_;
};

} // namespace zenthrildb
