#include "recovery/RecoveryEngine.hpp"

#include "wal/LogPayload.hpp"

#include <map>

namespace zenthrildb {

RecoveryPlan RecoveryEngine::analyze(const RecoveryContext& context) const {
  RecoveryPlan plan;
  LogSequenceNumber checkpoint{0};
  std::map<TransactionId, TransactionRecoveryStatus> transactionStatuses;
  for (const auto& record : context.records()) {
    if (record.type() == LogRecordType::Checkpoint) {
      checkpoint = record.lsn();
      plan.setCheckpointLsn(checkpoint);
    }
    switch (record.type()) {
      case LogRecordType::TransactionStarted:
        transactionStatuses.insert_or_assign(
          LogPayloadCodec::decodeTransaction(record.payload()).transactionId,
          TransactionRecoveryStatus::Incomplete);
        break;
      case LogRecordType::TransactionCommitted:
        transactionStatuses.insert_or_assign(
          LogPayloadCodec::decodeTransaction(record.payload()).transactionId,
          TransactionRecoveryStatus::Committed);
        break;
      case LogRecordType::TransactionRolledBack:
        transactionStatuses.insert_or_assign(
          LogPayloadCodec::decodeTransaction(record.payload()).transactionId,
          TransactionRecoveryStatus::RolledBack);
        break;
      default:
        break;
    }
  }
  for (const auto& [id, status] : transactionStatuses) {
    plan.setTransactionStatus(id, status);
  }

  for (const auto& record : context.records()) {
    if (record.lsn() < checkpoint) {
      continue;
    }
    auto actionType = classify(record.type());
    if (record.type() == LogRecordType::PageImageWritten) {
      const auto pageImage = LogPayloadCodec::decodePageImageWritten(record.payload());
      const auto status = transactionStatuses.at(pageImage.transactionId);
      actionType = status == TransactionRecoveryStatus::Committed
        ? RecoveryActionType::RedoCandidate
        : status == TransactionRecoveryStatus::Incomplete
          ? RecoveryActionType::UndoCandidate
          : RecoveryActionType::Ignore;
    }
    plan.addAction(RecoveryAction{
      .lsn = record.lsn(),
      .recordType = record.type(),
      .actionType = actionType,
      .payload = record.payload(),
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
    case LogRecordType::PageImageWritten:
      return RecoveryActionType::Ignore;
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
