#pragma once

#include "wal/RecoveryContext.hpp"
#include "wal/WriteAheadLog.hpp"

#include <filesystem>
#include <mutex>
#include <span>
#include <vector>

namespace zenthrildb {

class LogManager final : public RecoveryManager, public CheckpointManager {
public:
  explicit LogManager(std::filesystem::path walPath);

  [[nodiscard]] LogSequenceNumber append(LogRecordType type, std::span<const Byte> payload = {});
  void flush();
  [[nodiscard]] LogSequenceNumber checkpoint();
  [[nodiscard]] std::vector<LogRecord> replay();
  void truncate(LogSequenceNumber inclusiveMaxLsn);

  [[nodiscard]] RecoveryContext prepareRecovery() override;
  [[nodiscard]] LogSequenceNumber createCheckpoint() override;

private:
  [[nodiscard]] LogSequenceNumber nextLsn();

  mutable std::mutex mutex_;
  WriteAheadLog wal_;
  LogSequenceNumber nextLsn_{1};
};

} // namespace zenthrildb
