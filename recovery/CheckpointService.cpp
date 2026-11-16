#include "recovery/CheckpointService.hpp"
#include "core/FileManager.hpp"
#include "core/PageManager.hpp"
#include "recovery/RecoveryCoordinator.hpp"
#include "recovery/RecoveryEngine.hpp"
#include "storage/IoFaultInjector.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogPayload.hpp"

#include <stdexcept>

namespace zenthrildb {

CheckpointService::CheckpointService(LogManager& logManager)
  : logManager_(logManager) {}

LogSequenceNumber CheckpointService::checkpoint() {
  return logManager_.checkpoint();
}

CheckpointResult CheckpointService::checkpoint(FileManager& files, PageManager& pages,
                                               TransactionManager& transactions) {
  const auto& identity = files.walIdentity();
  if (!identity) throw std::logic_error("Safe checkpoint requires a bound database; create a snapshot first");
  if (transactions.activeCount() != 0) throw std::logic_error("Cannot checkpoint with active transactions");
  IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointPreparing);

  const auto context = logManager_.prepareRecovery();
  const auto records = context.records();
  const auto boundary = records.empty() ? LogSequenceNumber{0} : records.back().lsn();
  RecoveryCoordinator{}.verifyCheckpointState(files, logManager_, context, boundary);
  std::uint64_t txHighWater = 0;
  for (const auto& record : records) {
    if (record.type() == LogRecordType::TransactionStarted) {
      txHighWater = std::max(txHighWater, LogPayloadCodec::decodeTransaction(record.payload()).transactionId.value());
    }
  }

  logManager_.flush();
  IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointWalDurable);
  pages.flushDirtyPages(); // Runtime pages, headers and metadata share one data file.
  IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointPagesDurable);
  const auto lsn = logManager_.publishCheckpoint(SafeCheckpointPayload{
    .databaseUuid = identity->databaseUuid,
    .generation = identity->generation,
    .durableWalLsn = boundary.value(),
    .transactionHighWater = txHighWater,
  });
  return {lsn, boundary, txHighWater};
}

} // namespace zenthrildb
