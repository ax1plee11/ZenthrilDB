#include "transaction/TransactionLogAdapter.hpp"

#include "wal/LogPayload.hpp"

namespace zenthrildb {

TransactionLogAdapter::TransactionLogAdapter(LogManager& logManager) noexcept
  : logManager_(logManager) {}

LogSequenceNumber TransactionLogAdapter::recordBegin(TransactionId id) {
  return logManager_.append(LogRecordType::TransactionStarted,
                            LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
}

LogSequenceNumber TransactionLogAdapter::recordCommit(TransactionId id) {
  return logManager_.append(LogRecordType::TransactionCommitted,
                            LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
}

LogSequenceNumber TransactionLogAdapter::recordRollback(TransactionId id) {
  return logManager_.append(LogRecordType::TransactionRolledBack,
                            LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = id}));
}

} // namespace zenthrildb
