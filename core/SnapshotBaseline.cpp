#include "core/SnapshotBaseline.hpp"

#include "core/MetadataManager.hpp"
#include "core/PageManager.hpp"
#include "core/RecordManager.hpp"
#include "core/StorageConsistencyChecker.hpp"
#include "wal/LogPayload.hpp"
#include "wal/RecoveryContext.hpp"

#include <algorithm>

namespace zenthrildb {

bool SnapshotBaseline::existsFor(const std::filesystem::path& database) {
  return std::filesystem::exists(snapshotManifestPath(database)) ||
         std::filesystem::exists(snapshotBaselinePath(database));
}

SnapshotBaseline::SnapshotBaseline(const std::filesystem::path& database)
  : manifest_(readSnapshotManifest(snapshotManifestPath(database))) {
  const auto file = snapshotBaselinePath(database);
  baseline_.openReadOnly(file);
  if (baseline_.walIdentity() != std::optional{manifest_.identity} ||
      baseline_.header().totalPages != manifest_.pageCount ||
      std::filesystem::file_size(file) != manifest_.databaseBytes) {
    throw std::runtime_error("Snapshot baseline identity or size does not match its manifest");
  }
  if (snapshotFileCrc(file, manifest_.databaseBytes) != manifest_.databaseCrc) {
    throw std::runtime_error("Snapshot baseline checksum mismatch");
  }
  validatePages(baseline_, manifest_.identity.snapshotLsn);
}

void SnapshotBaseline::validatePages(FileManager& files, std::uint64_t lastLsn) {
  files.verifyPhysicalLayout();
  FreePageManager freePages(files);
  freePages.load();
  MetadataManager metadata(files, freePages);
  metadata.loadMetadata();
  StorageConsistencyChecker{}.checkPages(files, freePages, metadata, lastLsn);
  PageManager pages(files, freePages, metadata);
  std::recursive_mutex mutex;
  RecordManager(pages, metadata, mutex).validate();
}

void SnapshotBaseline::validateWal(const std::filesystem::path& path, const RecoveryContext& context) const {
  const auto boundary = manifest_.identity.snapshotLsn;
  std::uint64_t prefixBytes = WalIdentity::kSize;
  std::uint64_t highWater = 0;
  bool found = boundary == 0;
  const bool reduced = manifest_.identity.reducedHistory;
  if (reduced) {
    if (context.records().empty()) throw std::runtime_error("Reduced WAL is missing its baseline boundary");
    const auto& anchor = context.records().front();
    if (anchor.type() != LogRecordType::SafeCheckpoint || anchor.lsn().value() != boundary + 1) {
      throw std::runtime_error("Reduced WAL has an invalid baseline boundary");
    }
    const auto checkpoint = LogPayloadCodec::decodeSafeCheckpoint(anchor.payload());
    if (checkpoint.databaseUuid != manifest_.identity.databaseUuid ||
        checkpoint.generation != manifest_.identity.generation || checkpoint.durableWalLsn != boundary) {
      throw std::runtime_error("Reduced WAL boundary identity mismatch");
    }
    highWater = checkpoint.transactionHighWater;
    found = true;
  }
  for (const auto& record : context.records()) {
    const auto lsn = record.lsn().value();
    if (lsn <= boundary || (reduced && &record == &context.records().front())) {
      const auto frameSize = LogRecord::kHeaderSize + record.payload().size();
      if (frameSize > manifest_.walPrefixBytes - prefixBytes) {
        throw std::runtime_error("Snapshot WAL prefix length mismatch");
      }
      prefixBytes += frameSize;
      found |= lsn == boundary;
    }
    if (record.type() == LogRecordType::TransactionStarted) {
      const auto transaction = LogPayloadCodec::decodeTransaction(record.payload()).transactionId.value();
      if (lsn <= boundary) highWater = std::max(highWater, transaction);
      else if (transaction <= manifest_.transactionHighWater) {
        throw std::runtime_error("Post-snapshot transaction ID overlaps the baseline");
      }
    }
  }
  if (!found || prefixBytes != manifest_.walPrefixBytes || highWater != manifest_.transactionHighWater ||
      snapshotFileCrc(path, prefixBytes) != manifest_.walPrefixCrc) {
    throw std::runtime_error("Snapshot WAL prefix or transaction boundary mismatch");
  }
}

} // namespace zenthrildb
