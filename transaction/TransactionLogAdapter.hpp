#pragma once

#include "core/Types.hpp"
#include "transaction/TransactionId.hpp"
#include "wal/LogManager.hpp"

#include <stdexcept>
#include <vector>

namespace zenthrildb {

class CommitOutcomeUnknown final : public std::runtime_error {
public:
  CommitOutcomeUnknown()
    : std::runtime_error("Commit record was appended but WAL durability is unknown") {}
};

class TransactionLogAdapter {
public:
  explicit TransactionLogAdapter(LogManager& logManager) noexcept;

  [[nodiscard]] LogSequenceNumber recordBegin(TransactionId id);
  [[nodiscard]] LogSequenceNumber recordCommit(TransactionId id);
  [[nodiscard]] LogSequenceNumber recordRollback(TransactionId id);

private:
  LogManager& logManager_;
};

} // namespace zenthrildb
