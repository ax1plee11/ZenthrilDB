#include "wal/WriteAheadLog.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/IoFaultInjector.hpp"
#include "wal/LogPayload.hpp"
#include "storage/SafeFilePath.hpp"
#include "storage/WalInstallation.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace zenthrildb {

WriteAheadLog::WriteAheadLog(std::filesystem::path path, WalLimits limits) : limits_(limits) {
  if (limits_.maxBytes < LogRecord::kHeaderSize || limits_.maxRecords == 0) {
    throw std::invalid_argument("WAL limits must permit at least one frame");
  }
  open(path);
}

WriteAheadLog::~WriteAheadLog() {
  try { close(); } catch (...) { }
}

void WriteAheadLog::open(const std::filesystem::path& path) {
  open(path, true);
}

void WriteAheadLog::open(const std::filesystem::path& path, bool allowCreate, bool readOnly,
                         std::shared_ptr<DatabaseFileLock> ownership) {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) close();
  const auto physical = std::filesystem::absolute(path).lexically_normal();
  auto database = physical; database.replace_extension(".zdb");
  std::shared_ptr<DatabaseFileLock> databaseOwner;
  if (std::filesystem::exists(database)) {
    if (std::filesystem::exists(std::filesystem::symlink_status(walInstallationPath(database)))) {
      throw std::logic_error("Direct WAL access to an installed source is forbidden");
    }
    if (ownership && ownership->databasePath() != database) throw std::logic_error("WAL ownership belongs to another database");
    databaseOwner = ownership ? std::move(ownership) : std::make_shared<DatabaseFileLock>(database);
    (void)databaseOwner->guard();
  } else if (ownership) throw std::logic_error("WAL ownership requires its existing database");
  auto walOwner = std::make_shared<DatabaseFileLock>(physical, false);
  path_ = path;
  readOnly_ = readOnly;
  incompleteTailRepaired_ = false;
  incompleteTailOffset_.reset();
  hasIdentity_ = false;
  recordCount_.reset();
  detail::validateStoragePath(path_, allowCreate);
  std::error_code existsError;
  const auto alreadyExists = std::filesystem::exists(path_, existsError);
  if (existsError) {
    throw std::system_error(existsError, "Failed to inspect WAL path");
  }
  createdOnOpen_ = !alreadyExists;
  if (!alreadyExists) {
    if (!allowCreate) throw std::runtime_error("Database WAL is missing");
    detail::createStorageFileExclusive(path_);
    detail::syncFileToStorage(path_);
    detail::syncDirectoryToStorage(path_.parent_path());
  }
  stream_.open(path_, std::ios::binary | std::ios::in | (readOnly_ ? std::ios::openmode{} : std::ios::out));
  if (!stream_) {
    throw std::runtime_error("Failed to open existing WAL file without truncation");
  }
  databaseOwner_ = std::move(databaseOwner);
  walOwner_ = std::move(walOwner);
}

void WriteAheadLog::close() {
  std::lock_guard lock(mutex_);
  const auto retained = databaseOwner_ ? databaseOwner_ : walOwner_;
  auto ownership = retained ? retained->cleanupGuard() : std::unique_lock<std::recursive_mutex>{};
  if (stream_.is_open()) {
    if (!readOnly_) {
      stream_.flush();
      if (!stream_) throw std::runtime_error("Failed to flush WAL before close");
      detail::syncFileToStorage(path_);
    }
    stream_.close();
    if (stream_.fail()) throw std::runtime_error("Failed to close WAL file");
  }
  walOwner_.reset();
  databaseOwner_.reset();
}

std::unique_lock<std::recursive_mutex> WriteAheadLog::ownershipGuard() const {
  if (databaseOwner_) return databaseOwner_->guard();
  if (walOwner_) return walOwner_->guard();
  throw std::logic_error("WAL has no ownership");
}

void WriteAheadLog::append(const LogRecord& record) {
  std::lock_guard lock(mutex_);
  auto ownership = ownershipGuard();
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot append to a read-only WAL");
  if (incompleteTailOffset_) throw std::runtime_error("WAL has an incomplete tail");
  if (!recordCount_) (void)readAll({}, nullptr, false);
  if (incompleteTailOffset_) throw std::runtime_error("WAL has an incomplete tail");
  if (*recordCount_ >= limits_.maxRecords) throw std::length_error("WAL record limit reached; preserve and rotate the database offline");
  stream_.seekp(0, std::ios::end);
  if (!stream_) throw std::runtime_error("Failed to seek to WAL end");
  const auto end = stream_.tellp();
  if (end < 0) throw std::runtime_error("Cannot determine WAL length");
  const auto size = static_cast<std::uintmax_t>(end);
  const auto frameBytes = static_cast<std::uintmax_t>(LogRecord::kHeaderSize) + record.payload().size();
  if (size > limits_.maxBytes || frameBytes > limits_.maxBytes - size) {
    throw std::length_error("WAL byte limit reached; preserve and rotate the database offline");
  }
  const auto bytes = record.serialize();
  IoFaultInjector::failIfArmed(IoFaultPoint::NoSpace);
  IoFaultInjector::failIfArmed(IoFaultPoint::WalWrite);
  const bool checkpointPartial = record.type() == LogRecordType::SafeCheckpoint &&
    IoFaultInjector::isArmed(IoFaultPoint::CheckpointPartialFrame);
  if (checkpointPartial || IoFaultInjector::consume(IoFaultPoint::WalPartialWrite)) {
    const auto partialSize = std::max<std::size_t>(1, bytes.size() / 2);
    stream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(partialSize));
    stream_.flush();
    if (!stream_) throw std::runtime_error("Failed to write partial WAL frame");
    if (checkpointPartial) IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointPartialFrame);
    throw std::system_error(std::make_error_code(std::errc::io_error), "Injected partial WAL write");
  }
  stream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!stream_) {
    throw std::runtime_error("Failed to append WAL record");
  }
  ++*recordCount_;
}

std::vector<LogRecord> WriteAheadLog::readAll() {
  return readAll({}, nullptr, true);
}

std::vector<LogRecord> WriteAheadLog::readAll(
    const std::function<void(const LogRecord&)>& validate,
    const std::optional<WalIdentity>* expectedIdentity, bool repair) {
  std::lock_guard lock(mutex_);
  auto ownership = ownershipGuard();
  ensureOpen();
  stream_.flush();
  if (!stream_) {
    throw std::runtime_error("Failed to flush WAL before replay");
  }
  if (std::filesystem::file_size(path_) > limits_.maxBytes) {
    throw std::length_error("WAL exceeds replay byte limit");
  }
  stream_.clear();
  stream_.seekg(0, std::ios::beg);
  if (!stream_) throw std::runtime_error("Failed to seek to WAL start");
  const auto identity = readWalIdentity(stream_);
  if (expectedIdentity && identity != *expectedIdentity) {
    if (!identity || !*expectedIdentity) throw std::runtime_error("Database and WAL identity formats do not match");
    if (identity->databaseUuid != (*expectedIdentity)->databaseUuid) throw std::runtime_error("WAL database UUID mismatch");
    throw std::runtime_error("WAL snapshot generation or boundary mismatch");
  }
  hasIdentity_ = identity.has_value();
  incompleteTailOffset_.reset();

  std::vector<LogRecord> records;
  std::uintmax_t validBytes = identity ? WalIdentity::kSize : 0;
  const bool reduced = identity && identity->reducedHistory;
  bool anchorSeen = !reduced;
  auto snapshotLsnSeen = reduced || !identity || identity->snapshotLsn == 0;
  std::uint64_t previousLsn = reduced ? identity->snapshotLsn : 0;
  std::uint64_t transactionHighWater = 0;
  const auto noteTail = [&] {
    if (!anchorSeen) throw std::runtime_error("Reduced WAL boundary record is missing or incomplete");
    if (!snapshotLsnSeen) throw std::runtime_error("WAL does not reach the snapshot LSN");
    incompleteTailOffset_ = validBytes;
    recordCount_ = records.size();
    if (repair && !identity) repairPendingTail();
    else stream_.clear();
  };
  while (true) {
    std::array<Byte, LogRecord::kHeaderSize> header{};
    stream_.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto bytesRead = stream_.gcount();
    if (stream_.bad() || (stream_.fail() && !stream_.eof())) {
      throw std::runtime_error("Failed to read WAL record header");
    }
    if (bytesRead == 0) {
      break;
    }
    if (bytesRead != static_cast<std::streamsize>(header.size())) {
      noteTail();
      return records;
    }
    if (records.size() >= limits_.maxRecords) throw std::length_error("WAL exceeds replay record limit");

    std::size_t offset = 0;
    const auto magic = binary::readLittleEndian<std::uint32_t>(header, offset);
    const auto version = binary::readLittleEndian<std::uint32_t>(header, offset);
    const auto type = binary::readLittleEndian<std::uint32_t>(header, offset);
    const auto lsn = binary::readLittleEndian<std::uint64_t>(header, offset);
    offset += sizeof(std::uint64_t);
    const auto payloadSize = binary::readLittleEndian<std::uint32_t>(header, offset);
    if (magic != LogRecord::kMagic || version != LogRecord::kFormatVersion) {
      throw std::runtime_error("Invalid WAL frame magic or version");
    }
    if (identity && (type < static_cast<std::uint32_t>(LogRecordType::DatabaseCreated) ||
                     type > static_cast<std::uint32_t>(LogRecordType::SafeCheckpoint))) {
      throw std::runtime_error("Unsupported bound WAL record type");
    }
    if (identity && (lsn <= previousLsn || lsn == std::numeric_limits<std::uint64_t>::max())) {
      throw std::runtime_error("Invalid bound WAL frame LSN sequence");
    }
    if (!anchorSeen && (type != static_cast<std::uint32_t>(LogRecordType::SafeCheckpoint) ||
        lsn != identity->snapshotLsn + 1)) {
      throw std::runtime_error("Invalid reduced WAL boundary record");
    }
    if (identity && type == static_cast<std::uint32_t>(LogRecordType::Checkpoint) && payloadSize != 0) {
      throw std::runtime_error("Invalid checkpoint payload for this WAL version");
    }
    if (type == static_cast<std::uint32_t>(LogRecordType::SafeCheckpoint) &&
        (!identity || payloadSize != LogPayloadCodec::kSafeCheckpointPayloadSize)) {
      throw std::runtime_error("Invalid safe checkpoint frame");
    }
    if (payloadSize > LogRecord::kMaxPayloadSize) {
      throw std::runtime_error("Corrupted WAL: payload exceeds maximum size");
    }
    if (validBytes > limits_.maxBytes || header.size() > limits_.maxBytes - validBytes ||
        payloadSize > limits_.maxBytes - validBytes - header.size()) {
      throw std::length_error("WAL frame exceeds replay byte limit");
    }
    std::vector<Byte> payload(payloadSize);
    if (payloadSize > 0) {
      stream_.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
      if (stream_.bad() || (stream_.fail() && !stream_.eof())) {
        throw std::runtime_error("Failed to read WAL record payload");
      }
      if (stream_.gcount() != static_cast<std::streamsize>(payload.size())) {
        noteTail();
        return records;
      }
    }
    auto record = LogRecord::deserializeHeaderAndPayload(header, payload);
    if (identity && record.type() == LogRecordType::TransactionStarted) {
      const auto tx = LogPayloadCodec::decodeTransaction(record.payload());
      transactionHighWater = std::max(transactionHighWater, tx.transactionId.value());
    }
    if (record.type() == LogRecordType::SafeCheckpoint) {
      const auto checkpoint = LogPayloadCodec::decodeSafeCheckpoint(record.payload());
      if (!anchorSeen) transactionHighWater = checkpoint.transactionHighWater;
      if (checkpoint.databaseUuid != identity->databaseUuid ||
          (lsn > identity->snapshotLsn && checkpoint.generation != identity->generation) ||
          checkpoint.durableWalLsn != previousLsn || checkpoint.transactionHighWater != transactionHighWater) {
        throw std::runtime_error("Safe checkpoint disagrees with WAL identity or its history");
      }
    }
    if (validate) validate(record);
    previousLsn = lsn;
    if (identity && record.lsn().value() == identity->snapshotLsn) snapshotLsnSeen = true;
    records.push_back(std::move(record));
    anchorSeen = true;
    validBytes += header.size() + payload.size();
  }
  if (!snapshotLsnSeen) throw std::runtime_error("WAL does not reach the snapshot LSN");
  if (!anchorSeen) throw std::runtime_error("Reduced WAL boundary record is missing");
  stream_.clear();
  recordCount_ = records.size();
  return records;
}

void WriteAheadLog::flush() {
  std::lock_guard lock(mutex_);
  auto ownership = ownershipGuard();
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot synchronize a read-only WAL");
  stream_.flush();
  if (!stream_) {
    throw std::runtime_error("Failed to flush WAL stream");
  }

  IoFaultInjector::failIfArmed(IoFaultPoint::WalSync);
  detail::syncFileToStorage(path_);
}

void WriteAheadLog::truncate(LogSequenceNumber inclusiveMaxLsn) {
  std::lock_guard lock(mutex_);
  auto ownership = ownershipGuard();
  ensureOpen();
  const auto records = readAll();
  if (hasIdentity_) throw std::logic_error("WAL reclamation is disabled for bound databases");

  auto tempPath = path_;
  tempPath += ".rewrite.tmp";
  detail::createStorageFileExclusive(tempPath);

  {
    std::ofstream replacement(tempPath, std::ios::binary | std::ios::trunc);
    if (!replacement) throw std::runtime_error("Failed to create WAL rewrite file");
    for (const auto& record : records) {
      const auto lifecycle = record.type() == LogRecordType::TransactionStarted ||
        record.type() == LogRecordType::TransactionCommitted ||
        record.type() == LogRecordType::TransactionRolledBack;
      if (record.lsn().value() > inclusiveMaxLsn.value() || lifecycle) {
        const auto bytes = record.serialize();
        replacement.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!replacement) throw std::runtime_error("Failed to write WAL rewrite file");
      }
    }
    replacement.flush();
    if (!replacement) throw std::runtime_error("Failed to flush WAL rewrite file");
  }
  detail::syncFileToStorage(tempPath);
  stream_.clear();
  stream_.close();
#ifdef _WIN32
  if (!MoveFileExW(tempPath.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto error = GetLastError();
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    throw std::system_error(static_cast<int>(error), std::system_category(), "Failed to atomically replace WAL");
  }
#else
  std::error_code renameError;
  std::filesystem::rename(tempPath, path_, renameError);
  if (renameError) {
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    throw std::system_error(renameError, "Failed to atomically replace WAL");
  }
  detail::syncDirectoryToStorage(path_.parent_path());
#endif
  stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
  if (!stream_) throw std::runtime_error("Failed to reopen WAL after atomic replacement");
  recordCount_.reset();
}

void WriteAheadLog::repairPendingTail() {
  std::lock_guard lock(mutex_);
  auto ownership = ownershipGuard();
  if (!incompleteTailOffset_) return;
  if (readOnly_) throw std::logic_error("Cannot repair a read-only WAL");
  truncateIncompleteTail(*incompleteTailOffset_);
  incompleteTailOffset_.reset();
}

void WriteAheadLog::truncateIncompleteTail(std::uintmax_t validBytes) {
  incompleteTailRepaired_ = true;
  stream_.clear();
  stream_.close();
  std::error_code resizeError;
  std::filesystem::resize_file(path_, validBytes, resizeError);
  if (resizeError) {
    throw std::system_error(resizeError, "Failed to discard incomplete WAL tail");
  }
  stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
  if (!stream_) {
    throw std::runtime_error("Failed to reopen WAL after trimming incomplete tail");
  }
  detail::syncFileToStorage(path_);
}

void WriteAheadLog::ensureOpen() const {
  if (!stream_.is_open()) {
    throw std::runtime_error("WAL file is not open");
  }
}

} // namespace zenthrildb
