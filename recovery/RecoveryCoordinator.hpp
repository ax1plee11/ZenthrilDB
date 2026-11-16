#pragma once

#include "recovery/RecoveryExecutor.hpp"
#include "recovery/RecoveryPlan.hpp"
#include "storage/SnapshotManifest.hpp"

namespace zenthrildb {

class FileManager;
class LogManager;
class SnapshotBaseline;
class RecoveryContext;

struct RecoverySummary {
  RecoveryExecutionResult execution{};
  LogSequenceNumber lastWalLsn{};
  bool truncatedIncompleteWalTail{false};
};

// Explicit startup helper. Call before exposing the database to application work.
class RecoveryCoordinator final {
public:
  [[nodiscard]] RecoverySummary recover(LogManager& logManager, FileManager& fileManager) const;
  std::optional<SnapshotManifest> verifyCheckpointState(FileManager& fileManager, LogManager& logManager,
                                                       const RecoveryContext& context, LogSequenceNumber lastLsn) const;

private:
  [[nodiscard]] RecoveryExecutionResult recoverBound(
    LogManager* repairWal, FileManager& fileManager, const RecoveryPlan& plan, LogSequenceNumber lastLsn,
    SnapshotBaseline* baseline) const;
};

} // namespace zenthrildb
