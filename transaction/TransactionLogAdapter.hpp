#pragma once

#include "core/Types.hpp"
#include "transaction/TransactionId.hpp"
#include "wal/LogManager.hpp"

#include <vector>

namespace zenthrildb {

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
