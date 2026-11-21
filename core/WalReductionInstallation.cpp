#include "core/DatabaseRuntime.hpp"
#include "core/SnapshotBaseline.hpp"
#include "recovery/RecoveryCoordinator.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/IoFaultInjector.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "wal/LogPayload.hpp"
#include <algorithm>

namespace zenthrildb {
void DatabaseRuntime::installReduction(const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& candidatePath) {
  const auto source = std::filesystem::absolute(sourcePath).lexically_normal();
  const auto candidate = std::filesystem::absolute(candidatePath).lexically_normal();
  if (source == candidate || source.extension() != ".zdb" || source.filename() != candidate.filename()) {
    throw std::invalid_argument("Installation requires separate database paths with the same filename");
  }
  {
    auto sourceLock = std::make_shared<DatabaseFileLock>(source);
    const auto installed = FileManager::resolveInstallation(source);
    if (installed) {
      if (installed->targetPath != candidate) throw std::runtime_error("Another generation is already installed");
      // A retry validates the chosen generation; it never installs over it.
    } else {
      auto candidateLock = std::make_shared<DatabaseFileLock>(candidate);
      if (std::filesystem::exists(std::filesystem::symlink_status(walInstallationPath(candidate)))) {
        throw std::runtime_error("An installed alias cannot be an installation candidate");
      }
      IoFaultInjector::failIfArmed(IoFaultPoint::InstallPrepared);
      FileManager original;
      original.openReadOnly(source);
      if (!original.walIdentity()) throw std::runtime_error("Migrate legacy source to a bound snapshot before installation");
      LogManager oldWal(databaseWalPath(source), original.header(), original.walIdentity(), true, sourceLock);
      const auto oldContext = oldWal.prepareRecovery();
      if (oldWal.hasPendingTail()) throw std::runtime_error("Recover the source before installation");
      const auto oldLsn = oldContext.records().empty() ? LogSequenceNumber{0} : oldContext.records().back().lsn();
      (void)RecoveryCoordinator{}.verifyCheckpointState(original, oldWal, oldContext, oldLsn);
      SnapshotBaseline::validatePages(original, oldLsn.value());
      FileManager reduced;
      reduced.openReadOnly(candidate);
      LogManager newWal(databaseWalPath(candidate), reduced.header(), reduced.walIdentity(), true, candidateLock);
      const auto context = newWal.prepareRecovery();
      if (newWal.hasPendingTail() || context.records().size() != 1) {
        throw std::runtime_error("Installation requires a pristine reduced candidate");
      }
      const auto manifest = RecoveryCoordinator{}.verifyCheckpointState(reduced, newWal, context, context.records().back().lsn());
      if (!manifest || !manifest->identity.reducedHistory || manifest->identity.snapshotLsn != oldLsn.value() ||
          manifest->identity.databaseUuid != original.header().databaseUuid ||
          manifest->identity.generation == original.walIdentity()->generation) {
        throw std::runtime_error("Stale or incompatible installation candidate");
      }
      SnapshotBaseline::validatePages(reduced, context.records().back().lsn().value());
      std::uint64_t highWater = 0;
      for (const auto& record : oldContext.records()) {
        if (record.type() == LogRecordType::TransactionStarted) highWater = std::max(highWater,
          LogPayloadCodec::decodeTransaction(record.payload()).transactionId.value());
        if (record.type() == LogRecordType::SafeCheckpoint) highWater = std::max(highWater,
          LogPayloadCodec::decodeSafeCheckpoint(record.payload()).transactionHighWater);
      }
      if (highWater != manifest->transactionHighWater || original.header().serialize() != reduced.header().serialize()) {
        throw std::runtime_error("Installation candidate does not match source state");
      }
      for (std::uint64_t page = 1; page < original.header().totalPages; ++page) {
        if (original.readPage(static_cast<PageId>(page)).serialize() != reduced.readPage(static_cast<PageId>(page)).serialize()) {
          throw std::runtime_error("Installation candidate page differs from source");
        }
      }
      IoFaultInjector::failIfArmed(IoFaultPoint::InstallValidated);
      for (const auto& path : {databaseWalPath(candidate), candidate,
                               snapshotBaselinePath(candidate), snapshotManifestPath(candidate)}) {
        IoFaultInjector::failIfArmed(IoFaultPoint::InstallSync);
        detail::syncFileToStorage(path);
      }
      detail::syncDirectoryToStorage(candidate.parent_path());
      publishWalInstallation(source, WalInstallation{
        .source = *original.walIdentity(), .target = manifest->identity,
        .sourceDataBytes = std::filesystem::file_size(source),
        .sourceWalBytes = std::filesystem::file_size(databaseWalPath(source)),
        .sourceHasBaseline = SnapshotBaseline::existsFor(source), .targetPath = candidate,
      });
    }
  }
  // After the authority record is committed, ordinary open completes recovery.
  // An error here never restores an older source or erases the selector.
  IoFaultInjector::failIfArmed(IoFaultPoint::InstallOpen);
  DatabaseRuntime installed(source);
  (void)installed.checkConsistency();
  IoFaultInjector::failIfArmed(IoFaultPoint::InstallVerified);
}
}
