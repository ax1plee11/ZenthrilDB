#pragma once

#include "wal/LogManager.hpp"

namespace zenthrildb {

class DatabaseRuntime;
class FileManager;
class PageManager;
class TransactionManager;

struct CheckpointResult {
  LogSequenceNumber checkpointLsn;
  LogSequenceNumber durableWalLsn;
  std::uint64_t transactionHighWater{};
};

class CheckpointService {
public:
  explicit CheckpointService(LogManager& logManager);
  [[nodiscard]] LogSequenceNumber checkpoint();

private:
  friend class DatabaseRuntime;
  [[nodiscard]] CheckpointResult checkpoint(FileManager& files, PageManager& pages, TransactionManager& transactions);
  LogManager& logManager_;
};

} // namespace zenthrildb
