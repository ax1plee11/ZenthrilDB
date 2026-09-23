#pragma once

#include "transaction/TransactionId.hpp"

namespace zenthrildb {

class TransactionContext {
public:
  explicit TransactionContext(TransactionId id) noexcept : id_(id) {}

  [[nodiscard]] TransactionId transactionId() const noexcept { return id_; }
  [[nodiscard]] bool valid() const noexcept { return id_.valid(); }

private:
  TransactionId id_{};
};

} // namespace zenthrildb
