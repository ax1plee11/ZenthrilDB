#pragma once

#include "wal/LogRecord.hpp"
#include "wal/LogSequenceNumber.hpp"

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

struct RecoveryAction {
  LogSequenceNumber lsn{};
  LogRecordType recordType{LogRecordType::Unknown};
  RecoveryActionType actionType{RecoveryActionType::Ignore};
};

class RecoveryPlan {
public:
  void addAction(RecoveryAction action);

  [[nodiscard]] const std::vector<RecoveryAction>& actions() const noexcept { return actions_; }
  [[nodiscard]] LogSequenceNumber checkpointLsn() const noexcept { return checkpointLsn_; }
  void setCheckpointLsn(LogSequenceNumber lsn) noexcept { checkpointLsn_ = lsn; }
  [[nodiscard]] bool empty() const noexcept { return actions_.empty(); }

private:
  LogSequenceNumber checkpointLsn_{0};
  std::vector<RecoveryAction> actions_;
};

} // namespace zenthrildb
