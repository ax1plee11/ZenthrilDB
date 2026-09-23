#pragma once

#include "transaction/TransactionId.hpp"

#include <cstdint>

namespace zenthrildb {

enum class TransactionState {
  Active,
  Committed,
  RolledBack,
};

class Transaction {
public:
  Transaction() = default;
  Transaction(TransactionId id, std::uint64_t startedAtUnixNs);

  [[nodiscard]] TransactionId id() const noexcept { return id_; }
  [[nodiscard]] TransactionState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t startedAtUnixNs() const noexcept { return startedAtUnixNs_; }
  [[nodiscard]] std::uint64_t finishedAtUnixNs() const noexcept { return finishedAtUnixNs_; }

  void markCommitted(std::uint64_t finishedAtUnixNs) noexcept;
  void markRolledBack(std::uint64_t finishedAtUnixNs) noexcept;

private:
  TransactionId id_{};
  TransactionState state_{TransactionState::Active};
  std::uint64_t startedAtUnixNs_{0};
  std::uint64_t finishedAtUnixNs_{0};
};

} // namespace zenthrildb
