#pragma once

#include "transaction/Transaction.hpp"
#include "transaction/TransactionLogAdapter.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace zenthrildb {

class TransactionManager {
public:
  explicit TransactionManager(LogManager& logManager);

  [[nodiscard]] Transaction begin();
  void commit(TransactionId id);
  void rollback(TransactionId id);

  [[nodiscard]] std::optional<Transaction> getTransaction(TransactionId id) const;
  [[nodiscard]] bool isActive(TransactionId id) const;
  [[nodiscard]] std::size_t activeCount() const;
  [[nodiscard]] std::vector<Transaction> activeTransactions() const;

private:
  [[nodiscard]] static std::uint64_t currentTimeUnixNs();
  [[nodiscard]] Transaction& requireActive(TransactionId id);

  mutable std::mutex mutex_;
  TransactionLogAdapter logAdapter_;
  std::uint64_t nextId_{1};
  std::unordered_map<TransactionId, Transaction> transactions_;
};

} // namespace zenthrildb
