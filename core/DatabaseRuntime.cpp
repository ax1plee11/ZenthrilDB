#include "core/DatabaseRuntime.hpp"
#include "core/SnapshotBaseline.hpp"
#include "recovery/RecoveryCoordinator.hpp"

#include "storage/DatabaseWalPath.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/IoFaultInjector.hpp"
#include "wal/LogPayload.hpp"

#include <algorithm>
#include <stdexcept>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zenthrildb {
namespace {

void createPrivateDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
  if (!std::filesystem::create_directory(directory)) {
    throw std::invalid_argument("Destination directory already exists");
  }
#else
  if (::mkdir(directory.c_str(), 0700) == -1) {
    const auto error = errno;
    if (error == EEXIST) throw std::invalid_argument("Destination directory already exists");
    throw std::system_error(error, std::generic_category(), "Failed to create private directory");
  }
#endif
}

void publishFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Failed to publish database file");
  }
#else
  // The private destination directory was created exclusively for this operation.
  std::filesystem::rename(source, destination);
#endif
}

void publishSnapshot(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
  publishFile(source, destination);
#else
  if (::renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == -1) {
    throw std::system_error(errno, std::generic_category(), "Failed to publish snapshot directory");
  }
#endif
}

void copySnapshotFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                      IoFaultPoint partialPoint) {
  IoFaultInjector::failIfArmed(IoFaultPoint::NoSpace);
  if (!IoFaultInjector::isArmed(partialPoint)) {
    std::filesystem::copy_file(source, destination);
    return;
  }
  // Write a real short file before the process interruption.
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::out);
  std::array<char, 16> bytes{};
  if (!input || !output || !input.read(bytes.data(), bytes.size())) {
    throw std::runtime_error("Failed to prepare partial snapshot file");
  }
  output.write(bytes.data(), bytes.size());
  output.close();
  if (!output) throw std::runtime_error("Failed to write partial snapshot file");
  IoFaultInjector::failIfArmed(partialPoint);
  throw std::runtime_error("Incomplete snapshot copy");
}

} // namespace

DatabaseRuntime::DatabaseRuntime(const std::filesystem::path& path, bool create)
  : path_(std::filesystem::absolute(path)) {
  if (create) files_.createDatabase(path_, true);
  else files_.openDatabase(path_);

  wal_ = std::make_unique<LogManager>(databaseWalPath(path_), files_.header(), files_.walIdentity());
  transactions_ = std::make_unique<TransactionManager>(*wal_);
  mutations_ = std::make_unique<StorageMutationCoordinator>(files_, *wal_, *transactions_);
  freePages_ = std::make_unique<FreePageManager>(files_, *mutations_);
  freePages_->load();
  metadata_ = std::make_unique<MetadataManager>(files_, *freePages_, *mutations_);
  if (create) metadata_->saveMetadata();
  else metadata_->loadMetadata();
  pages_ = std::make_unique<PageManager>(files_, *freePages_, *metadata_);
  records_ = std::make_unique<RecordManager>(*pages_, *metadata_, operationsMutex_);
  records_->validate();
  (void)StorageConsistencyChecker{}.check(files_, *freePages_, *metadata_, *wal_);
}

StorageConsistencyReport DatabaseRuntime::checkConsistency() {
  std::lock_guard lock(operationsMutex_);
  records_->validate();
  return StorageConsistencyChecker{}.check(files_, *freePages_, *metadata_, *wal_);
}

CheckpointResult DatabaseRuntime::checkpoint() {
  std::lock_guard lock(operationsMutex_);
  if (!files_.walIdentity()) throw std::logic_error("Safe checkpoint requires a bound database; create a snapshot first");
  try {
    (void)checkConsistency();
    return CheckpointService(*wal_).checkpoint(files_, *pages_, *transactions_);
  } catch (...) {
    files_.markFailed();
    wal_->markFailed();
    throw;
  }
}

void DatabaseRuntime::backupTo(const std::filesystem::path& destinationDirectory) {
  std::lock_guard lock(operationsMutex_);
  (void)checkConsistency();
  wal_->flush();
  files_.flush();

  auto destination = std::filesystem::absolute(destinationDirectory).lexically_normal();
  if (!destination.has_filename() && destination.has_relative_path()) {
    destination = destination.parent_path();
  }
  if (std::filesystem::exists(destination)) throw std::invalid_argument("Destination directory already exists");

  const auto stagingToken = WalIdentity::newGeneration();
  constexpr char hex[] = "0123456789abcdef";
  std::string suffix = ".backup-";
  for (const auto byte : stagingToken) {
    suffix.push_back(hex[byte >> 4]);
    suffix.push_back(hex[byte & 0xf]);
  }
  auto staging = destination;
  staging += suffix;
  const auto databaseCopy = staging / path_.filename();
  const auto walCopy = databaseWalPath(databaseCopy);
  const auto baselineCopy = snapshotBaselinePath(databaseCopy);
  const auto manifestCopy = snapshotManifestPath(databaseCopy);
  auto lockCopy = databaseCopy;
  lockCopy += ".lock";
  const bool hasBaseline = SnapshotBaseline::existsFor(path_);
  createPrivateDirectory(staging);

  bool published = false;
  try {
    std::filesystem::copy_file(databaseWalPath(path_), walCopy);
    detail::syncFileToStorage(walCopy);
    std::filesystem::copy_file(path_, databaseCopy);
    detail::syncFileToStorage(databaseCopy);
    if (hasBaseline) {
      std::filesystem::copy_file(snapshotBaselinePath(path_), baselineCopy);
      detail::syncFileToStorage(baselineCopy);
      std::filesystem::copy_file(snapshotManifestPath(path_), manifestCopy);
      detail::syncFileToStorage(manifestCopy);
      (void)verifySnapshot(databaseCopy);
      std::filesystem::remove(lockCopy);
    }
    detail::syncDirectoryToStorage(staging);
    publishSnapshot(staging, destination);
    published = true;
    detail::syncDirectoryToStorage(destination.parent_path());
  } catch (...) {
    if (!published) {
      std::error_code ignored;
      for (const auto& path : {lockCopy, manifestCopy, baselineCopy, databaseCopy, walCopy, staging}) {
        std::filesystem::remove(path, ignored);
      }
    }
    throw;
  }
}

void DatabaseRuntime::snapshotTo(const std::filesystem::path& sourcePath,
                                 const std::filesystem::path& destinationDirectory) {
  const auto source = std::filesystem::absolute(sourcePath);
  auto destination = std::filesystem::absolute(destinationDirectory).lexically_normal();
  if (!destination.has_filename() && destination.has_relative_path()) destination = destination.parent_path();
  if (source.extension() != ".zdb") throw std::invalid_argument("Snapshot source must end in .zdb");
  const auto sourceWal = databaseWalPath(source);
  DatabaseFileLock sourceLock(source);
  if (!std::filesystem::is_regular_file(source) || !std::filesystem::is_regular_file(sourceWal)) {
    throw std::invalid_argument("Snapshot requires an existing database and its WAL");
  }

  if (std::filesystem::exists(destination)) throw std::invalid_argument("Destination directory already exists");
  const auto stagingToken = WalIdentity::newGeneration();
  constexpr char hex[] = "0123456789abcdef";
  std::string suffix = ".snapshot-";
  for (const auto byte : stagingToken) {
    suffix.push_back(hex[byte >> 4]);
    suffix.push_back(hex[byte & 0xf]);
  }
  auto staging = destination;
  staging += suffix;
  const auto stagedData = staging / source.filename();
  const auto stagedWal = databaseWalPath(stagedData);
  const auto newWal = staging / "identity-wal.partial";
  const auto baselinePath = snapshotBaselinePath(stagedData);
  const auto manifestPath = snapshotManifestPath(stagedData);
  auto stagedLock = stagedData;
  stagedLock += ".lock";
  IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotCreateDirectory);
  createPrivateDirectory(staging);
  try {
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotDirectory);
    copySnapshotFile(source, stagedData, IoFaultPoint::SnapshotDataPartial);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotDataCopied);
    copySnapshotFile(sourceWal, stagedWal, IoFaultPoint::SnapshotWalPartial);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotWalCopied);
    if (SnapshotBaseline::existsFor(source)) {
      std::filesystem::copy_file(snapshotBaselinePath(source), baselinePath);
      std::filesystem::copy_file(snapshotManifestPath(source), manifestPath);
    }

    SnapshotManifest manifest;
    {
      DatabaseRuntime verified(stagedData);
      const auto records = verified.wal_->replay();
      const auto boundary = records.empty() ? 0 : records.back().lsn().value();
      manifest.identity = WalIdentity::fresh(verified.files_.header(), boundary);
      manifest.pageCount = verified.files_.header().totalPages;
      for (const auto& record : records) {
        if (record.type() == LogRecordType::TransactionStarted) {
          manifest.transactionHighWater = std::max(manifest.transactionHighWater,
            LogPayloadCodec::decodeTransaction(record.payload()).transactionId.value());
        }
      }
      if (verified.files_.walIdentity() && manifest.identity.generation == verified.files_.walIdentity()->generation) {
        throw std::runtime_error("Snapshot generation was reused");
      }
    }
    std::filesystem::remove(baselinePath);
    std::filesystem::remove(manifestPath);
    {
      IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotOpenFile);
      std::ifstream oldWal(stagedWal, std::ios::binary);
      std::ofstream output(newWal, std::ios::binary | std::ios::out);
      if (!oldWal || !output) throw std::runtime_error("Failed to stage snapshot WAL");
      IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotReadFile);
      (void)readWalIdentity(oldWal);
      const auto bytes = manifest.identity.serialize();
      IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotWriteFile);
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      std::array<char, 64 * 1024> buffer{};
      while (oldWal.read(buffer.data(), buffer.size()) || oldWal.gcount() != 0) {
        output.write(buffer.data(), oldWal.gcount());
      }
      IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotCloseFile);
      output.close();
      if (oldWal.bad() || !oldWal.eof() || !output) throw std::runtime_error("Failed to copy snapshot WAL records");
    }
    std::filesystem::remove(stagedWal);
    publishFile(newWal, stagedWal);
    {
      std::fstream data(stagedData, std::ios::binary | std::ios::in | std::ios::out);
      const auto page = manifest.identity.toPage().serialize();
      data.seekp(DatabaseHeader::serializedSize());
      data.write(reinterpret_cast<const char*>(page.data()), page.size());
      data.close();
      if (!data) throw std::runtime_error("Failed to stage database snapshot identity");
    }
    std::filesystem::copy_file(stagedData, baselinePath);
    manifest.databaseBytes = std::filesystem::file_size(baselinePath);
    manifest.walPrefixBytes = std::filesystem::file_size(stagedWal);
    manifest.databaseCrc = snapshotFileCrc(baselinePath, manifest.databaseBytes);
    manifest.walPrefixCrc = snapshotFileCrc(stagedWal, manifest.walPrefixBytes);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotSyncFile);
    detail::syncFileToStorage(stagedWal);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotWalDurable);
    detail::syncFileToStorage(stagedData);
    detail::syncFileToStorage(baselinePath);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotDataDurable);
    writeSnapshotManifest(manifestPath, manifest);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotManifestWritten);
    detail::syncFileToStorage(manifestPath);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotManifestDurable);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotValidating);
    (void)verifySnapshot(stagedData);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotSyncDirectory);
    detail::syncDirectoryToStorage(staging);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotPublish);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotRename);
    publishSnapshot(staging, destination);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotRenamed);
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotSyncParent);
    detail::syncDirectoryToStorage(destination.parent_path());
    IoFaultInjector::failIfArmed(IoFaultPoint::SnapshotParentDurable);
  } catch (...) {
    std::error_code ignored;
    for (const auto& path : {newWal, stagedWal, stagedData, stagedLock, baselinePath, manifestPath, staging}) {
      std::filesystem::remove(path, ignored);
    }
    throw;
  }
}

SnapshotManifest DatabaseRuntime::verifySnapshot(const std::filesystem::path& path, bool baselineOnly) {
  if (baselineOnly) return SnapshotBaseline(path).manifest();
  DatabaseFileLock lock(path);
  if (!SnapshotBaseline::existsFor(path)) throw std::runtime_error("Snapshot manifest and baseline are missing");
  FileManager files;
  files.openReadOnly(path);
  LogManager wal(databaseWalPath(path), files.header(), files.walIdentity(), true);
  const auto context = wal.prepareRecovery();
  if (wal.hasPendingTail()) throw std::runtime_error("Snapshot verification found an incomplete WAL tail");
  const auto& records = context.records();
  const auto last = records.empty() ? LogSequenceNumber{0} : records.back().lsn();
  const auto manifest = RecoveryCoordinator{}.verifyCheckpointState(files, wal, context, last);
  if (!manifest) throw std::runtime_error("Snapshot files disappeared during verification");
  SnapshotBaseline::validatePages(files, last.value());
  return *manifest;
}

} // namespace zenthrildb
