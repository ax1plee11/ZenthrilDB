#include "transaction/TransactionLogAdapter.hpp"

#include "wal/LogPayload.hpp"

#include <exception>

namespace zenthrildb {

TransactionLogAdapter::TransactionLogAdapter(LogManager& logManager) noexcept
  : logManager_(logManager) {}

LogSequenceNumber TransactionLogAdapter::recordBegin(TransactionId id) {
  return logManager_.appendWithLsn(LogRecordType::TransactionStarted, [id](LogSequenceNumber) {
    return LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id});
  });
}

LogSequenceNumber TransactionLogAdapter::recordCommit(TransactionId id) {
  try {
    return logManager_.appendWithLsn(LogRecordType::TransactionCommitted, [id](LogSequenceNumber) {
      return LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id});
    });
  } catch (...) {
    std::throw_with_nested(CommitOutcomeUnknown{});
  }
}

LogSequenceNumber TransactionLogAdapter::recordRollback(TransactionId id) {
  try {
    return logManager_.appendWithLsn(LogRecordType::TransactionRolledBack, [id](LogSequenceNumber) {
      return LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id});
    });
  } catch (...) {
    std::throw_with_nested(RollbackOutcomeUnknown{});
  }
}

} // namespace zenthrildb
