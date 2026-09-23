#pragma once

#include "recovery/RecoveryPlan.hpp"

#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace zenthrildb {

class RedoApplier {
public:
  virtual ~RedoApplier() = default;
  virtual void applyRedo(const RecoveryAction& action) = 0;
};

class UndoApplier {
public:
  virtual ~UndoApplier() = default;
  virtual void applyUndo(const RecoveryAction& action) = 0;
};

struct RecoveryExecutionResult {
  std::size_t redoApplied{0};
  std::size_t undoApplied{0};
  std::size_t skipped{0};
  LogSequenceNumber lastAppliedLsn{0};
};

class UndoRedoRecoveryExecutor {
public:
  UndoRedoRecoveryExecutor(RedoApplier& redoApplier, UndoApplier& undoApplier);

  [[nodiscard]] RecoveryExecutionResult execute(const RecoveryPlan& plan);
  void resetIdempotencyState();

private:
  [[nodiscard]] bool shouldSkip(const RecoveryAction& action) const noexcept;
  [[nodiscard]] bool alreadyApplied(LogSequenceNumber lsn) const;
  void rememberApplied(LogSequenceNumber lsn);

  mutable std::mutex mutex_;
  RedoApplier& redoApplier_;
  UndoApplier& undoApplier_;
  std::unordered_set<std::uint64_t> appliedLsns_;
};

} // namespace zenthrildb
