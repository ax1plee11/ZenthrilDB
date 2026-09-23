#include "transaction/TransactionManager.hpp"

#include <chrono>
#include <stdexcept>

namespace zenthrildb {

TransactionManager::TransactionManager(LogManager& logManager)
  : logAdapter_(logManager) {}

Transaction TransactionManager::begin() {
  std::lock_guard lock(mutex_);
  const TransactionId id{nextId_++};
  Transaction transaction{id, currentTimeUnixNs()};
  transactions_.emplace(id, transaction);
  try {
    (void)logAdapter_.recordBegin(id);
  } catch (...) {
    transactions_.erase(id);
    --nextId_;
    throw;
  }
  return transaction;
}

void TransactionManager::commit(TransactionId id) {
  std::lock_guard lock(mutex_);
  auto& transaction = requireActive(id);
  (void)logAdapter_.recordCommit(id);
  transaction.markCommitted(currentTimeUnixNs());
}

void TransactionManager::rollback(TransactionId id) {
  std::lock_guard lock(mutex_);
  auto& transaction = requireActive(id);
  (void)logAdapter_.recordRollback(id);
  transaction.markRolledBack(currentTimeUnixNs());
}

std::optional<Transaction> TransactionManager::getTransaction(TransactionId id) const {
  std::lock_guard lock(mutex_);
  const auto it = transactions_.find(id);
  if (it == transactions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool TransactionManager::isActive(TransactionId id) const {
  const auto transaction = getTransaction(id);
  return transaction.has_value() && transaction->state() == TransactionState::Active;
}

std::size_t TransactionManager::activeCount() const {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  for (const auto& [id, transaction] : transactions_) {
    (void)id;
    if (transaction.state() == TransactionState::Active) {
      ++count;
    }
  }
  return count;
}

std::vector<Transaction> TransactionManager::activeTransactions() const {
  std::lock_guard lock(mutex_);
  std::vector<Transaction> active;
  for (const auto& [id, transaction] : transactions_) {
    (void)id;
    if (transaction.state() == TransactionState::Active) {
      active.push_back(transaction);
    }
  }
  return active;
}

std::uint64_t TransactionManager::currentTimeUnixNs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

Transaction& TransactionManager::requireActive(TransactionId id) {
  const auto it = transactions_.find(id);
  if (it == transactions_.end()) {
    throw std::runtime_error("Transaction does not exist");
  }
  if (it->second.state() != TransactionState::Active) {
    throw std::runtime_error("Transaction is not active");
  }
  return it->second;
}

} // namespace zenthrildb
