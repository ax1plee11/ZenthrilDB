#include "recovery/RecoveryPlan.hpp"

#include <utility>

namespace zenthrildb {

void RecoveryPlan::addAction(RecoveryAction action) {
  actions_.push_back(std::move(action));
}

void RecoveryPlan::setTransactionStatus(TransactionId id, TransactionRecoveryStatus status) {
  transactionStatuses_.insert_or_assign(id, status);
}

} // namespace zenthrildb
