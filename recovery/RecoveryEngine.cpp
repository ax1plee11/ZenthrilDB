#include "recovery/RecoveryEngine.hpp"

namespace zenthrildb {

RecoveryPlan RecoveryEngine::analyze(const RecoveryContext& context) const {
  RecoveryPlan plan;
  LogSequenceNumber checkpoint{0};
  for (const auto& record : context.records()) {
    if (record.type() == LogRecordType::Checkpoint) {
      checkpoint = record.lsn();
      plan.setCheckpointLsn(checkpoint);
    }
  }

  for (const auto& record : context.records()) {
    if (record.lsn() < checkpoint) {
      continue;
    }
    plan.addAction(RecoveryAction{
      .lsn = record.lsn(),
      .recordType = record.type(),
      .actionType = classify(record.type()),
    });
  }
  return plan;
}

void RecoveryEngine::recover(const RecoveryContext& context) {
  lastPlan_ = analyze(context);
}

RecoveryActionType RecoveryEngine::classify(LogRecordType type) const noexcept {
  switch (type) {
    case LogRecordType::PageAllocated:
    case LogRecordType::PageFreed:
    case LogRecordType::PageWritten:
      return RecoveryActionType::RedoCandidate;
    case LogRecordType::MetadataUpdated:
      return RecoveryActionType::MetadataRefresh;
    case LogRecordType::Checkpoint:
      return RecoveryActionType::CheckpointBoundary;
    case LogRecordType::RecoveryMarker:
      return RecoveryActionType::RecoveryMarker;
    case LogRecordType::TransactionStarted:
    case LogRecordType::TransactionCommitted:
    case LogRecordType::TransactionRolledBack:
      return RecoveryActionType::TransactionBoundary;
    case LogRecordType::DatabaseCreated:
    case LogRecordType::DatabaseOpened:
    case LogRecordType::Unknown:
    default:
      return RecoveryActionType::Ignore;
  }
}

} // namespace zenthrildb
