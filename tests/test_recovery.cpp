#include "recovery/CheckpointService.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "recovery/RecoveryExecutor.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

using namespace zenthrildb;

namespace {

std::filesystem::path tempRecoveryWalPath() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_recovery.wal";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  return path;
}

} // namespace

void test_recovery() {
  const auto path = tempRecoveryWalPath();

  {
    LogManager log(path);
    (void)log.append(LogRecordType::DatabaseCreated);
    (void)log.append(LogRecordType::PageWritten);
    CheckpointService checkpoints(log);
    const auto checkpoint = checkpoints.checkpoint();
    assert(checkpoint == LogSequenceNumber{3});
    (void)log.append(LogRecordType::MetadataUpdated);
    (void)log.append(LogRecordType::RecoveryMarker);
    log.flush();

    RecoveryEngine engine;
    auto context = log.prepareRecovery();
    const auto plan = engine.analyze(context);
    assert(plan.checkpointLsn() == LogSequenceNumber{3});
    assert(plan.actions().size() == 3);
    assert(plan.actions()[0].actionType == RecoveryActionType::CheckpointBoundary);
    assert(plan.actions()[1].actionType == RecoveryActionType::MetadataRefresh);
    assert(plan.actions()[2].actionType == RecoveryActionType::RecoveryMarker);

    engine.recover(context);
    assert(engine.lastPlan().actions().size() == 3);
  }

  std::filesystem::remove(path);

  {
    class CountingRedoApplier final : public RedoApplier {
    public:
      void applyRedo(const RecoveryAction& action) override {
        std::lock_guard lock(mutex_);
        appliedLsns.push_back(action.lsn.value());
      }

      std::mutex mutex_;
      std::vector<std::uint64_t> appliedLsns;
    };

    class CountingUndoApplier final : public UndoApplier {
    public:
      void applyUndo(const RecoveryAction& action) override {
        std::lock_guard lock(mutex_);
        appliedLsns.push_back(action.lsn.value());
      }

      std::mutex mutex_;
      std::vector<std::uint64_t> appliedLsns;
    };

    RecoveryPlan plan;
    plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{1}, .recordType = LogRecordType::Checkpoint,
                                  .actionType = RecoveryActionType::CheckpointBoundary});
    plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{2}, .recordType = LogRecordType::PageWritten,
                                  .actionType = RecoveryActionType::RedoCandidate});
    plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{3}, .recordType = LogRecordType::MetadataUpdated,
                                  .actionType = RecoveryActionType::MetadataRefresh});
    plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{4}, .recordType = LogRecordType::Unknown,
                                  .actionType = RecoveryActionType::UndoCandidate});
    plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{5}, .recordType = LogRecordType::TransactionCommitted,
                                  .actionType = RecoveryActionType::TransactionBoundary});

    CountingRedoApplier redo;
    CountingUndoApplier undo;
    UndoRedoRecoveryExecutor executor(redo, undo);

    const auto first = executor.execute(plan);
    assert(first.redoApplied == 2);
    assert(first.undoApplied == 1);
    assert(first.skipped == 2);
    assert(first.lastAppliedLsn == LogSequenceNumber{4});
    assert(redo.appliedLsns.size() == 2);
    assert(undo.appliedLsns.size() == 1);

    const auto second = executor.execute(plan);
    assert(second.redoApplied == 0);
    assert(second.undoApplied == 0);
    assert(second.skipped == 5);
    assert(redo.appliedLsns.size() == 2);
    assert(undo.appliedLsns.size() == 1);

    executor.resetIdempotencyState();
    const auto afterReset = executor.execute(plan);
    assert(afterReset.redoApplied == 2);
    assert(afterReset.undoApplied == 1);
  }

  {
    class RaceRedoApplier final : public RedoApplier {
    public:
      void applyRedo(const RecoveryAction& action) override {
        std::lock_guard lock(mutex_);
        appliedLsns.push_back(action.lsn.value());
      }

      std::mutex mutex_;
      std::vector<std::uint64_t> appliedLsns;
    };

    class NoopUndoApplier final : public UndoApplier {
    public:
      void applyUndo(const RecoveryAction&) override {}
    };

    RecoveryPlan plan;
    for (std::uint64_t lsn = 10; lsn < 20; ++lsn) {
      plan.addAction(RecoveryAction{.lsn = LogSequenceNumber{lsn}, .recordType = LogRecordType::PageWritten,
                                    .actionType = RecoveryActionType::RedoCandidate});
    }

    RaceRedoApplier redo;
    NoopUndoApplier undo;
    UndoRedoRecoveryExecutor executor(redo, undo);
    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&] {
        (void)executor.execute(plan);
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }

    assert(redo.appliedLsns.size() == 10);
  }
}
