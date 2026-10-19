#pragma once

#include "wal/LogRecord.hpp"
#include "wal/LogSequenceNumber.hpp"
#include "transaction/TransactionId.hpp"

#include <map>
#include <vector>

namespace zenthrildb {

enum class RecoveryActionType {
  RedoCandidate,
  UndoCandidate,
  MetadataRefresh,
  CheckpointBoundary,
  RecoveryMarker,
  TransactionBoundary,
  Ignore,
};

enum class TransactionRecoveryStatus {
  Incomplete,
  Committed,
  RolledBack,
};

struct RecoveryAction {
  LogSequenceNumber lsn{};
  LogRecordType recordType{LogRecordType::Unknown};
  RecoveryActionType actionType{RecoveryActionType::Ignore};
  std::vector<Byte> payload{};
};

class RecoveryPlan {
public:
  void addAction(RecoveryAction action);
  void setTransactionStatus(TransactionId id, TransactionRecoveryStatus status);

  [[nodiscard]] const std::vector<RecoveryAction>& actions() const noexcept { return actions_; }
  [[nodiscard]] const std::map<TransactionId, TransactionRecoveryStatus>& transactionStatuses() const noexcept {
    return transactionStatuses_;
  }
  [[nodiscard]] LogSequenceNumber checkpointLsn() const noexcept { return checkpointLsn_; }
  void setCheckpointLsn(LogSequenceNumber lsn) noexcept { checkpointLsn_ = lsn; }
  [[nodiscard]] bool empty() const noexcept { return actions_.empty(); }

private:
  LogSequenceNumber checkpointLsn_{0};
  std::vector<RecoveryAction> actions_;
  std::map<TransactionId, TransactionRecoveryStatus> transactionStatuses_;
};

} // namespace zenthrildb
