#pragma once

#include "wal/RecoveryContext.hpp"
#include "wal/WriteAheadLog.hpp"

#include <filesystem>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace zenthrildb {

class LogManager final : public RecoveryManager, public CheckpointManager {
public:
  explicit LogManager(std::filesystem::path walPath);

  [[nodiscard]] LogSequenceNumber append(LogRecordType type, std::span<const Byte> payload = {});

  // Reserves an LSN before payload construction and durably flushes the appended record.
  // Factory, append, and flush exceptions propagate; a reserved LSN is not reused.
  template <typename PayloadFactory>
  [[nodiscard]] LogSequenceNumber appendWithLsn(LogRecordType type, PayloadFactory&& payloadFactory) {
    std::lock_guard lock(mutex_);
    const auto assignedLsn = nextLsn();
    auto payloadBytes = std::forward<PayloadFactory>(payloadFactory)(assignedLsn);
    wal_.append(LogRecord{assignedLsn, type, std::move(payloadBytes)});
    wal_.flush();
    return assignedLsn;
  }

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
