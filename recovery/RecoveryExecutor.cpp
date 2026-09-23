#include "recovery/RecoveryExecutor.hpp"

namespace zenthrildb {

UndoRedoRecoveryExecutor::UndoRedoRecoveryExecutor(RedoApplier& redoApplier, UndoApplier& undoApplier)
  : redoApplier_(redoApplier), undoApplier_(undoApplier) {}

RecoveryExecutionResult UndoRedoRecoveryExecutor::execute(const RecoveryPlan& plan) {
  std::lock_guard lock(mutex_);
  RecoveryExecutionResult result;

  for (const auto& action : plan.actions()) {
    if (shouldSkip(action) || alreadyApplied(action.lsn)) {
      ++result.skipped;
      continue;
    }

    if (action.actionType == RecoveryActionType::RedoCandidate ||
        action.actionType == RecoveryActionType::MetadataRefresh) {
      redoApplier_.applyRedo(action);
      ++result.redoApplied;
      result.lastAppliedLsn = action.lsn;
      rememberApplied(action.lsn);
      continue;
    }

    if (action.actionType == RecoveryActionType::UndoCandidate) {
      undoApplier_.applyUndo(action);
      ++result.undoApplied;
      result.lastAppliedLsn = action.lsn;
      rememberApplied(action.lsn);
      continue;
    }

    ++result.skipped;
  }

  return result;
}

void UndoRedoRecoveryExecutor::resetIdempotencyState() {
  std::lock_guard lock(mutex_);
  appliedLsns_.clear();
}

bool UndoRedoRecoveryExecutor::shouldSkip(const RecoveryAction& action) const noexcept {
  return action.lsn.value() == 0 || action.actionType == RecoveryActionType::Ignore ||
         action.actionType == RecoveryActionType::CheckpointBoundary ||
         action.actionType == RecoveryActionType::RecoveryMarker ||
         action.actionType == RecoveryActionType::TransactionBoundary;
}

bool UndoRedoRecoveryExecutor::alreadyApplied(LogSequenceNumber lsn) const {
  return appliedLsns_.contains(lsn.value());
}

void UndoRedoRecoveryExecutor::rememberApplied(LogSequenceNumber lsn) {
  appliedLsns_.insert(lsn.value());
}

} // namespace zenthrildb
