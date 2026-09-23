#pragma once

#include "wal/LogRecord.hpp"

#include <vector>

namespace zenthrildb {

class RecoveryContext {
public:
  void addRecord(LogRecord record);
  [[nodiscard]] const std::vector<LogRecord>& records() const noexcept { return records_; }
  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

private:
  std::vector<LogRecord> records_;
};

class RecoveryManager {
public:
  virtual ~RecoveryManager() = default;
  virtual RecoveryContext prepareRecovery() = 0;
};

class CrashRecovery {
public:
  virtual ~CrashRecovery() = default;
  virtual void recover(const RecoveryContext& context) = 0;
};

class CheckpointManager {
public:
  virtual ~CheckpointManager() = default;
  virtual LogSequenceNumber createCheckpoint() = 0;
};

} // namespace zenthrildb
