#include "recovery/RecoveryEngine.hpp"

#include "storage/DatabaseHeader.hpp"
#include "storage/BinaryIO.hpp"
#include "storage/Page.hpp"
#include "wal/LogPayload.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {
namespace {

struct PhysicalMutation {
  LogSequenceNumber lsn;
  TransactionId transactionId;
  std::optional<PageId> pageId;
  bool newlyAllocated{false};
};

} // namespace

RecoveryPlan RecoveryEngine::analyze(const RecoveryContext& context) const {
  RecoveryPlan plan;
  LogSequenceNumber checkpoint{0};
  std::map<TransactionId, TransactionRecoveryStatus> transactionStatuses;
  std::map<TransactionId, std::uint64_t> initialPageCounts;
  std::vector<PhysicalMutation> physicalMutations;
  for (const auto& record : context.records()) {
    if (record.type() == LogRecordType::Checkpoint) {
      checkpoint = record.lsn();
      plan.setCheckpointLsn(checkpoint);
    }
    if (record.type() == LogRecordType::TransactionStarted) {
      const auto id = LogPayloadCodec::decodeTransaction(record.payload()).transactionId;
      if (transactionStatuses.contains(id)) {
        throw std::runtime_error("Corrupted WAL: duplicate transaction start");
      }
      transactionStatuses.emplace(id, TransactionRecoveryStatus::Incomplete);
      continue;
    }
    if (record.type() == LogRecordType::TransactionCommitted ||
        record.type() == LogRecordType::TransactionRolledBack) {
      const auto id = LogPayloadCodec::decodeTransaction(record.payload()).transactionId;
      const auto it = transactionStatuses.find(id);
      if (it == transactionStatuses.end() || it->second != TransactionRecoveryStatus::Incomplete) {
        throw std::runtime_error("Corrupted WAL: transaction finalized without one active start");
      }
      it->second = record.type() == LogRecordType::TransactionCommitted
        ? TransactionRecoveryStatus::Committed
        : TransactionRecoveryStatus::RolledBack;
      continue;
    }
    if (record.type() == LogRecordType::PageImageWritten ||
        record.type() == LogRecordType::DatabaseHeaderImageWritten) {
      PhysicalMutation mutation{.lsn = record.lsn(), .transactionId = {}, .pageId = std::nullopt};
      if (record.type() == LogRecordType::PageImageWritten) {
        const auto payload = LogPayloadCodec::decodePageImageWritten(record.payload());
        auto beforeLsnOffset = Page::kV2PageLsnOffset;
        auto afterLsnOffset = Page::kV2PageLsnOffset;
        if (binary::readLittleEndian<std::uint64_t>(payload.beforeImage, beforeLsnOffset) >= record.lsn().value() ||
            binary::readLittleEndian<std::uint64_t>(payload.afterImage, afterLsnOffset) != record.lsn().value() ||
            payload.previousTxLsn >= record.lsn().value()) {
          throw std::runtime_error("Corrupted WAL: inconsistent page image LSN");
        }
        mutation.transactionId = payload.transactionId;
        mutation.pageId = payload.pageId;
        const auto initial = initialPageCounts.find(mutation.transactionId);
        mutation.newlyAllocated = initial != initialPageCounts.end() && payload.pageId >= initial->second;
      } else {
        const auto payload = LogPayloadCodec::decodeDatabaseHeaderImageWritten(record.payload());
        if (payload.previousTxLsn >= record.lsn().value()) {
          throw std::runtime_error("Corrupted WAL: inconsistent database header image LSN");
        }
        mutation.transactionId = payload.transactionId;
        DatabaseHeader before;
        before.deserialize(payload.beforeImage);
        initialPageCounts.try_emplace(mutation.transactionId, before.totalPages);
      }
      const auto it = transactionStatuses.find(mutation.transactionId);
      if (it == transactionStatuses.end() || it->second != TransactionRecoveryStatus::Incomplete) {
        throw std::runtime_error("Corrupted WAL: page image is outside an active transaction");
      }
      physicalMutations.push_back(mutation);
    }
  }
  for (const auto& [id, status] : transactionStatuses) {
    plan.setTransactionStatus(id, status);
  }

  // A later committed image determines the durable state of the same target.
  // An older loser may still appear in the WAL after an earlier recovery.
  std::map<LogSequenceNumber, RecoveryActionType> physicalActions;
  std::unordered_set<PageId> laterCommittedPages;
  bool laterCommittedHeader = false;
  for (auto it = physicalMutations.rbegin(); it != physicalMutations.rend(); ++it) {
    const auto& mutation = *it;
    const auto committed = transactionStatuses.at(mutation.transactionId) == TransactionRecoveryStatus::Committed;
    if (committed) {
      physicalActions.emplace(mutation.lsn, RecoveryActionType::RedoCandidate);
      if (mutation.pageId) laterCommittedPages.insert(*mutation.pageId);
      else laterCommittedHeader = true;
    } else {
      const auto superseded = mutation.pageId
        ? laterCommittedPages.contains(*mutation.pageId) || (mutation.newlyAllocated && laterCommittedHeader)
        : laterCommittedHeader;
      physicalActions.emplace(mutation.lsn, superseded ? RecoveryActionType::Ignore : RecoveryActionType::UndoCandidate);
    }
  }

  for (const auto& record : context.records()) {
    auto actionType = classify(record.type());
    if (const auto it = physicalActions.find(record.lsn()); it != physicalActions.end()) {
      actionType = it->second;
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
    case LogRecordType::DatabaseHeaderImageWritten:
      return RecoveryActionType::Ignore;
    case LogRecordType::MetadataUpdated:
      return RecoveryActionType::MetadataRefresh;
    case LogRecordType::Checkpoint:
    case LogRecordType::SafeCheckpoint:
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
