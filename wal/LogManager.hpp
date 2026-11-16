#pragma once

#include "wal/RecoveryContext.hpp"
#include "wal/WriteAheadLog.hpp"
#include "storage/DatabaseHeader.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace zenthrildb {

class RecoveryCoordinator;
class CheckpointService;
class DatabaseRuntime;
struct SafeCheckpointPayload;

class LogManager final : public RecoveryManager, public CheckpointManager {
public:
  explicit LogManager(std::filesystem::path walPath);
  LogManager(std::filesystem::path walPath, const std::array<Byte, DatabaseHeader::kUuidSize>& databaseUuid);
  LogManager(std::filesystem::path walPath, const DatabaseHeader& header, std::optional<WalIdentity> identity,
             bool readOnly = false);

  [[nodiscard]] LogSequenceNumber append(LogRecordType type, std::span<const Byte> payload = {});

  // Reserves an LSN before payload construction and durably flushes the appended record.
  // Factory, append, and flush exceptions propagate; a reserved LSN is not reused.
  template <typename PayloadFactory>
  [[nodiscard]] LogSequenceNumber appendWithLsn(LogRecordType type, PayloadFactory&& payloadFactory) {
    std::lock_guard lock(mutex_);
    if (type == LogRecordType::SafeCheckpoint) throw std::invalid_argument("Use the runtime checkpoint service");
    ensureWritable();
    const auto assignedLsn = nextLsn();
    auto payloadBytes = std::forward<PayloadFactory>(payloadFactory)(assignedLsn);
    try {
      wal_.append(LogRecord{assignedLsn, type, std::move(payloadBytes)});
      wal_.flush();
    } catch (...) {
      poisoned_ = true;
      throw;
    }
    return assignedLsn;
  }

  void flush();
  [[nodiscard]] LogSequenceNumber checkpoint();
  [[nodiscard]] std::vector<LogRecord> replay();
  void truncate(LogSequenceNumber inclusiveMaxLsn);
  [[nodiscard]] bool walWasCreated() const noexcept { return wal_.wasCreated(); }
  [[nodiscard]] bool hadIncompleteWalTail() const noexcept { return wal_.hadIncompleteTail(); }
  [[nodiscard]] bool hasPendingTail() const noexcept { return wal_.incompleteTailOffset_.has_value(); }

  [[nodiscard]] RecoveryContext prepareRecovery() override;
  [[nodiscard]] LogSequenceNumber createCheckpoint() override;

private:
  friend class RecoveryCoordinator;
  friend class CheckpointService;
  friend class DatabaseRuntime;
  void markFailed();
  [[nodiscard]] LogSequenceNumber publishCheckpoint(const SafeCheckpointPayload& payload);
  [[nodiscard]] bool isBound() const noexcept { return wal_.hasIdentity_; }
  LogManager(std::filesystem::path walPath, std::optional<std::array<Byte, DatabaseHeader::kUuidSize>> databaseUuid);
  [[nodiscard]] std::vector<LogRecord> readValidated();
  void repairPendingTail() { wal_.repairPendingTail(); }
  [[nodiscard]] LogSequenceNumber nextLsn();
  void ensureWritable() const;

  mutable std::mutex mutex_;
  std::optional<std::array<Byte, DatabaseHeader::kUuidSize>> databaseUuid_;
  std::optional<WalIdentity> identity_;
  bool paired_{false};
  WriteAheadLog wal_;
  LogSequenceNumber nextLsn_{1};
  bool poisoned_{false};
};

} // namespace zenthrildb
