#include "transaction/TransactionLogAdapter.hpp"

#include "wal/LogPayload.hpp"

#include <exception>

namespace zenthrildb {

TransactionLogAdapter::TransactionLogAdapter(LogManager& logManager) noexcept
  : logManager_(logManager) {}

LogSequenceNumber TransactionLogAdapter::recordBegin(TransactionId id) {
  return logManager_.append(LogRecordType::TransactionStarted,
                            LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
}

LogSequenceNumber TransactionLogAdapter::recordCommit(TransactionId id) {
  const auto lsn = logManager_.append(
    LogRecordType::TransactionCommitted,
    LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
  try {
    logManager_.flush();
  } catch (...) {
    std::throw_with_nested(CommitOutcomeUnknown{});
  }
  return lsn;
}

LogSequenceNumber TransactionLogAdapter::recordRollback(TransactionId id) {
  return logManager_.append(LogRecordType::TransactionRolledBack,
                            LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
}

} // namespace zenthrildb
