#include "transaction/Transaction.hpp"

namespace zenthrildb {

Transaction::Transaction(TransactionId id, std::uint64_t startedAtUnixNs)
  : id_(id), startedAtUnixNs_(startedAtUnixNs) {}

void Transaction::markCommitted(std::uint64_t finishedAtUnixNs) noexcept {
  state_ = TransactionState::Committed;
  finishedAtUnixNs_ = finishedAtUnixNs;
}

void Transaction::markRolledBack(std::uint64_t finishedAtUnixNs) noexcept {
  state_ = TransactionState::RolledBack;
  finishedAtUnixNs_ = finishedAtUnixNs;
}

} // namespace zenthrildb
