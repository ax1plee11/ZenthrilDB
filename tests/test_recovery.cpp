#include "recovery/CheckpointService.hpp"
#include "recovery/PageImageRecoveryApplier.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "recovery/RecoveryExecutor.hpp"
#include "core/FileManager.hpp"
#include "storage/Page.hpp"
#include "wal/LogPayload.hpp"
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

std::filesystem::path tempRecoveryDatabasePath() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_page_image_recovery.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  return path;
}

Page makeRecoveryPage(PageId pageId, Byte value, std::uint64_t lsn) {
  Page page;
  page.header().version = Page::kFormatVersionV2;
  page.header().pageId = pageId;
  page.header().pageType = PageType::Table;
  page.header().usedBytes = 1;
  page.payload()[0] = value;
  page.setPageLsn(lsn);
  return page;
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
    RecoveryContext context;
    const auto transactionRecord = [](std::uint64_t lsn, LogRecordType type, std::uint64_t id) {
      return LogRecord{LogSequenceNumber{lsn}, type,
                       LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = TransactionId{id}})};
    };
    context.addRecord(transactionRecord(1, LogRecordType::TransactionStarted, 41));
    context.addRecord(transactionRecord(2, LogRecordType::TransactionCommitted, 41));
    context.addRecord(transactionRecord(3, LogRecordType::TransactionStarted, 42));
    context.addRecord(transactionRecord(4, LogRecordType::TransactionRolledBack, 42));
    context.addRecord(transactionRecord(5, LogRecordType::TransactionStarted, 43));
    context.addRecord(LogRecord{LogSequenceNumber{6}, LogRecordType::Checkpoint, {}});

    RecoveryEngine engine;
    const auto plan = engine.analyze(context);
    assert(plan.transactionStatuses().at(TransactionId{41}) == TransactionRecoveryStatus::Committed);
    assert(plan.transactionStatuses().at(TransactionId{42}) == TransactionRecoveryStatus::RolledBack);
    assert(plan.transactionStatuses().at(TransactionId{43}) == TransactionRecoveryStatus::Incomplete);
  }

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

  {
    const auto databasePath = tempRecoveryDatabasePath();
    FileManager files;
    files.createDatabase(databasePath);

    const auto before = makeRecoveryPage(kFirstUserPageId, 0x11, 10);
    const auto after = makeRecoveryPage(kFirstUserPageId, 0x22, 20);
    files.writePage(before);
    files.writePage(after); // Simulates the data page having reached disk before a crash.

    const auto pageImageRecord = [](std::uint64_t lsn, TransactionId transactionId) {
      const auto before = makeRecoveryPage(kFirstUserPageId, 0x11, 10);
      const auto after = makeRecoveryPage(kFirstUserPageId, 0x22, 20);
      return LogRecord{LogSequenceNumber{lsn}, LogRecordType::PageImageWritten,
                       LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
                         .transactionId = transactionId,
                         .pageId = kFirstUserPageId,
                         .previousTxLsn = 0,
                         .pageFormatVersion = Page::kFormatVersionV2,
                         .beforeImage = before.serialize(),
                         .afterImage = after.serialize(),
                       })};
    };
    const auto recoveryTransactionRecord = [](std::uint64_t lsn, LogRecordType type, TransactionId transactionId) {
      return LogRecord{LogSequenceNumber{lsn}, type,
                       LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = transactionId})};
    };

    RecoveryContext undoContext;
    undoContext.addRecord(recoveryTransactionRecord(1, LogRecordType::TransactionStarted, TransactionId{501}));
    undoContext.addRecord(pageImageRecord(2, TransactionId{501}));
    RecoveryEngine engine;
    const auto undoPlan = engine.analyze(undoContext);
    assert(undoPlan.actions()[1].actionType == RecoveryActionType::UndoCandidate);
    PageImageRecoveryApplier applier(files);
    UndoRedoRecoveryExecutor executor(applier, applier);
    const auto undoResult = executor.execute(undoPlan);
    assert(undoResult.undoApplied == 1);
    assert(files.readPage(kFirstUserPageId).payload()[0] == 0x11);

    RecoveryContext redoContext;
    redoContext.addRecord(recoveryTransactionRecord(3, LogRecordType::TransactionStarted, TransactionId{502}));
    redoContext.addRecord(pageImageRecord(4, TransactionId{502}));
    redoContext.addRecord(recoveryTransactionRecord(5, LogRecordType::TransactionCommitted, TransactionId{502}));
    const auto redoPlan = engine.analyze(redoContext);
    assert(redoPlan.actions()[1].actionType == RecoveryActionType::RedoCandidate);
    const auto redoResult = executor.execute(redoPlan);
    assert(redoResult.redoApplied == 1);
    assert(files.readPage(kFirstUserPageId).payload()[0] == 0x22);

    files.closeDatabase();
    std::filesystem::remove(databasePath);
  }
}
