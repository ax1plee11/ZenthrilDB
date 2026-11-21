#include "recovery/RecoveryCoordinator.hpp"

#include "core/FileManager.hpp"
#include "core/SnapshotBaseline.hpp"
#include "recovery/PageImageRecoveryApplier.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "storage/IoFaultInjector.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "wal/LogManager.hpp"
#include "wal/LogPayload.hpp"

#include <stdexcept>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace zenthrildb {
namespace {

bool validPageType(PageId id, PageType type) {
  if (id <= kPageDirectoryPageId) return type == PageType::Metadata;
  if (id == kFreePageListPageId) return type == PageType::Free;
  switch (type) {
    case PageType::Metadata:
    case PageType::Table:
    case PageType::Index:
    case PageType::Free:
    case PageType::Overflow:
    case PageType::Reserved:
      return true;
  }
  return false;
}

Page imagePage(const PageImageWrittenPayload& payload, bool undo) {
  Page page;
  page.deserialize(undo ? payload.beforeImage : payload.afterImage);
  if (!validPageType(payload.pageId, page.header().pageType)) {
    throw std::runtime_error("WAL page image has an invalid page type");
  }
  return page;
}

struct RecoveryPage {
  const RecoveryAction* action;
  bool undo;
};

RecoveryPlan physicalPlanFor(const RecoveryContext& context, SnapshotBaseline* baseline) {
  RecoveryContext suffix;
  if (baseline) {
    for (const auto& record : context.records()) {
      if (record.lsn().value() > baseline->manifest().identity.snapshotLsn) suffix.addRecord(record);
    }
  }
  RecoveryEngine engine;
  const auto plan = engine.analyze(baseline ? suffix : context);
  RecoveryPlan physicalPlan;
  for (const auto& action : plan.actions()) {
    if (action.recordType == LogRecordType::MetadataUpdated) {
      (void)LogPayloadCodec::decodeMetadataUpdated(action.payload);
      continue; // Informational metadata record. Physical images carry changes.
    }
    if (action.actionType == RecoveryActionType::RedoCandidate &&
        action.recordType != LogRecordType::PageImageWritten &&
        action.recordType != LogRecordType::DatabaseHeaderImageWritten) {
      throw std::runtime_error("Startup recovery requires physical WAL page images");
    }
    physicalPlan.addAction(action);
  }
  return physicalPlan;
}

std::unique_ptr<SnapshotBaseline> loadBaseline(const std::filesystem::path& database,
                                              const std::filesystem::path& wal,
                                              FileManager& files, const RecoveryContext& context) {
  if (!SnapshotBaseline::existsFor(database)) {
    if (files.walIdentity() && files.walIdentity()->baselineRequired) {
      throw std::runtime_error("This database generation requires its snapshot manifest and baseline");
    }
    return {};
  }
  auto baseline = std::make_unique<SnapshotBaseline>(database);
  if (files.walIdentity() != std::optional{baseline->manifest().identity}) {
    throw std::runtime_error("Snapshot manifest does not match the working database identity");
  }
  baseline->validateWal(wal, context);
  return baseline;
}

} // namespace

RecoverySummary RecoveryCoordinator::recover(LogManager& logManager, FileManager& fileManager) const {
  auto operation = fileManager.operation();
  if (fileManager.walIdentity() && std::filesystem::absolute(logManager.wal_.path()).lexically_normal() !=
      std::filesystem::absolute(databaseWalPath(fileManager.path_)).lexically_normal()) {
    throw std::logic_error("Recovery WAL does not belong to the owned database path");
  }
  if (logManager.isBound() != fileManager.walIdentity().has_value() ||
      (fileManager.walIdentity() && (!logManager.paired_ || logManager.identity_ != fileManager.walIdentity()))) {
    throw std::runtime_error("Recovery requires a WAL validated against the database identity");
  }
  const auto context = logManager.prepareRecovery();
  const auto& records = context.records();
  const auto lastLsn = records.empty() ? LogSequenceNumber{0} : records.back().lsn();
  auto baseline = loadBaseline(fileManager.path_, logManager.wal_.path(), fileManager, context);
  const auto physicalPlan = physicalPlanFor(context, baseline.get());
  RecoveryExecutionResult execution;
  if (fileManager.walIdentity()) {
    execution = recoverBound(&logManager, fileManager, physicalPlan, lastLsn, baseline.get());
    if (baseline) fileManager.recoverySnapshotBoundary_ = baseline->manifest().identity.snapshotLsn;
  } else {
    const auto physicalPageCount = fileManager.physicalPageCount();
    for (std::uint64_t rawPageId = 0; rawPageId < physicalPageCount; ++rawPageId) {
      const auto page = fileManager.readPage(static_cast<PageId>(rawPageId));
      if (page.header().pageId != rawPageId) throw std::runtime_error("Corrupted database: page id does not match its physical slot");
      if (page.header().version == Page::kFormatVersionV2 && page.pageLsn() > lastLsn.value()) {
        throw std::runtime_error("Corrupted database: page LSN is beyond the validated WAL");
      }
    }
    logManager.repairPendingTail();
    PageImageRecoveryApplier applier(fileManager);
    UndoRedoRecoveryExecutor executor(applier, applier);
    execution = executor.execute(physicalPlan);
  }
  fileManager.flush();
  fileManager.verifyPhysicalLayout();
  logManager.flush();
  return RecoverySummary{.execution = execution, .lastWalLsn = lastLsn,
                         .truncatedIncompleteWalTail = logManager.hadIncompleteWalTail()};
}

std::optional<SnapshotManifest> RecoveryCoordinator::verifyCheckpointState(
    FileManager& files, LogManager& logManager, const RecoveryContext& context, LogSequenceNumber lastLsn) const {
  auto baseline = loadBaseline(files.path_, logManager.wal_.path(), files, context);
  const auto plan = physicalPlanFor(context, baseline.get());
  (void)recoverBound(nullptr, files, plan, lastLsn, baseline.get());
  if (baseline) return baseline->manifest();
  return std::nullopt;
}

RecoveryExecutionResult RecoveryCoordinator::recoverBound(
    LogManager* repairWal, FileManager& files, const RecoveryPlan& plan, LogSequenceNumber lastLsn,
    SnapshotBaseline* baseline) const {
  const auto layout = files.physicalLayout();
  const auto physicalSlots = layout.completePages + layout.hasPartialPage;
  const auto diskHeader = files.header().serialize();
  auto finalHeader = baseline ? baseline->header().serialize() : diskHeader;
  auto baselinePages = baseline ? baseline->header().totalPages : files.header().totalPages;
  bool diskHeaderSeen = baseline && diskHeader == finalHeader;
  std::set<TransactionId> headerTransactions;
  std::map<TransactionId, std::pair<std::uint64_t, std::uint64_t>> discardedGrowth;
  std::map<PageId, RecoveryPage> selected;

  for (const auto& action : plan.actions()) {
    if (action.recordType != LogRecordType::DatabaseHeaderImageWritten) continue;
    const auto image = LogPayloadCodec::decodeDatabaseHeaderImageWritten(action.payload);
    DatabaseHeader before;
    DatabaseHeader after;
    before.deserialize(image.beforeImage);
    after.deserialize(image.afterImage);
    if (!baseline && headerTransactions.empty()) {
      finalHeader = image.beforeImage;
      baselinePages = before.totalPages;
    }
    if (image.beforeImage != finalHeader || after.totalPages <= before.totalPages ||
        after.creationTimestampUnixNs != before.creationTimestampUnixNs ||
        !headerTransactions.insert(image.transactionId).second) {
      throw std::runtime_error("Ambiguous database growth in WAL");
    }
    diskHeaderSeen |= diskHeader == image.beforeImage || diskHeader == image.afterImage;
    if (action.actionType == RecoveryActionType::RedoCandidate) finalHeader = image.afterImage;
    else discardedGrowth.emplace(image.transactionId, std::pair{before.totalPages, after.totalPages});
  }
  if ((baseline || !headerTransactions.empty()) && !diskHeaderSeen) {
    throw std::runtime_error("Database header is outside the recorded growth history");
  }
  DatabaseHeader target;
  target.deserialize(finalHeader);
  std::set<PageId> discardableTail;

  for (const auto& action : plan.actions()) {
    if (action.recordType != LogRecordType::PageImageWritten) continue;
    const auto image = LogPayloadCodec::decodePageImageWritten(action.payload);
    const auto id = image.pageId;
    const auto before = imagePage(image, true);
    (void)imagePage(image, false);
    if (id >= target.totalPages) {
      const auto growth = discardedGrowth.find(image.transactionId);
      if (action.actionType == RecoveryActionType::RedoCandidate || growth == discardedGrowth.end() ||
          id < growth->second.first || id >= growth->second.second) {
        throw std::runtime_error("WAL does not explain a page beyond the recovered database size");
      }
      discardableTail.insert(id);
      continue;
    }
    if (action.actionType == RecoveryActionType::Ignore) continue;
    const bool undo = action.actionType == RecoveryActionType::UndoCandidate;
    const auto previous = selected.find(id);
    if (previous != selected.end()) {
      const auto earlier = LogPayloadCodec::decodePageImageWritten(previous->second.action->payload);
      const auto& expected = previous->second.undo ? earlier.beforeImage : earlier.afterImage;
      if (image.beforeImage != expected) throw std::runtime_error("Ambiguous before-image for database page");
    } else if (baseline && id < baselinePages) {
      auto original = baseline->readPage(id);
      original.header().usedBytes = std::min<std::uint32_t>(original.header().usedBytes, Page::kV2PayloadSize);
      original.header().version = Page::kFormatVersionV2;
      if (original.serialize() != image.beforeImage) {
        throw std::runtime_error("WAL before-image disagrees with the snapshot baseline");
      }
    } else if (before.pageLsn() != 0) {
      throw std::runtime_error("WAL is missing the predecessor image for a database page");
    }
    selected.insert_or_assign(id, RecoveryPage{&action, undo});
  }

  if (physicalSlots > target.totalPages && physicalSlots - target.totalPages > discardableTail.size()) {
    throw std::runtime_error("WAL does not explain the excess database tail");
  }
  for (auto id = target.totalPages; id < physicalSlots; ++id) {
    if (!discardableTail.contains(static_cast<PageId>(id))) {
      throw std::runtime_error("WAL does not explain the excess database tail");
    }
  }
  const auto coveredPages = baseline ? std::max(layout.completePages, baselinePages) : layout.completePages;
  if (target.totalPages > coveredPages && target.totalPages - coveredPages > selected.size()) {
    throw std::runtime_error("Missing complete WAL image for a database page");
  }

  struct PageWrite {
    PageId id;
    bool damaged;
  };
  std::vector<PageWrite> writes;
  for (std::uint64_t slot = 0; slot < target.totalPages; ++slot) {
    const auto id = static_cast<PageId>(slot);
    const auto image = selected.find(id);
    if (slot >= baselinePages && image == selected.end()) {
      throw std::runtime_error("WAL is missing a committed image for a page allocated during growth");
    }
    std::optional<Page> persisted;
    if (slot < layout.completePages) {
      try { persisted = files.readPage(id); }
      catch (const PageChecksumError&) { if (id == kHeaderMirrorPageId) throw; }
    }
    if (persisted) {
      if (persisted->header().pageId != id || !validPageType(id, persisted->header().pageType)) {
        throw std::runtime_error("Database page has an invalid slot or type");
      }
      if (persisted->header().version == Page::kFormatVersionV2 && persisted->pageLsn() > lastLsn.value()) {
        throw std::runtime_error("Corrupted database: page LSN is beyond the validated WAL");
      }
    }
    if (image == selected.end()) {
      if (baseline && slot < baselinePages) {
        if (!persisted || persisted->serialize() != baseline->readPage(id).serialize()) {
          if (id == kHeaderMirrorPageId) throw std::runtime_error("Snapshot identity page differs from the baseline");
          writes.push_back({id, !persisted});
        }
        continue;
      }
      if (!persisted) throw std::runtime_error("Missing complete WAL image for damaged database page");
      continue;
    }
    const auto payload = LogPayloadCodec::decodePageImageWritten(image->second.action->payload);
    const auto& expected = image->second.undo ? payload.beforeImage : payload.afterImage;
    if (!persisted || persisted->serialize() != expected) writes.push_back({id, !persisted});
  }

  if (!repairWal) {
    if (!writes.empty() || diskHeader != finalHeader || layout.hasPartialPage || physicalSlots != target.totalPages) {
      throw std::runtime_error("Checkpoint requires the fully recovered database state");
    }
    return {};
  }
  repairWal->repairPendingTail();
  if (files.header().totalPages < target.totalPages) {
    // A crash after writing an individual page must leave a header from the recorded history.
    files.restoreHeaderImage(finalHeader, false);
  }
  RecoveryExecutionResult result;
  for (const auto& write : writes) {
    const auto entry = selected.find(write.id);
    if (entry == selected.end()) {
      files.writePage(baseline->readPage(write.id));
      ++result.redoApplied;
    } else {
      const auto image = LogPayloadCodec::decodePageImageWritten(entry->second.action->payload);
      files.writePage(imagePage(image, entry->second.undo));
      if (entry->second.undo) ++result.undoApplied;
      else ++result.redoApplied;
      result.lastAppliedLsn = entry->second.action->lsn;
    }
    if (write.damaged) {
      files.flush();
      IoFaultInjector::failIfArmed(IoFaultPoint::RecoveryPageRepair);
    }
  }
  if (files.header().serialize() != finalHeader || physicalSlots > target.totalPages) {
    files.restoreHeaderImage(finalHeader, physicalSlots > target.totalPages);
  }
  return result;
}

} // namespace zenthrildb
