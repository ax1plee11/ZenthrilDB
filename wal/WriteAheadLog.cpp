#include "wal/WriteAheadLog.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/IoFaultInjector.hpp"
#include "wal/LogPayload.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace zenthrildb {

WriteAheadLog::WriteAheadLog(std::filesystem::path path) {
  open(path);
}

WriteAheadLog::~WriteAheadLog() {
  try { close(); } catch (...) { }
}

void WriteAheadLog::open(const std::filesystem::path& path) {
  open(path, true);
}

void WriteAheadLog::open(const std::filesystem::path& path, bool allowCreate, bool readOnly) {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) {
    stream_.flush();
    if (!stream_) throw std::runtime_error("Failed to flush WAL before reopening");
    detail::syncFileToStorage(path_);
    stream_.close();
  }
  path_ = path;
  readOnly_ = readOnly;
  incompleteTailRepaired_ = false;
  incompleteTailOffset_.reset();
  hasIdentity_ = false;
  std::error_code existsError;
  const auto alreadyExists = std::filesystem::exists(path_, existsError);
  if (existsError) {
    throw std::system_error(existsError, "Failed to inspect WAL path");
  }
  createdOnOpen_ = !alreadyExists;
  if (!alreadyExists) {
    if (!allowCreate) throw std::runtime_error("Database WAL is missing");
    std::ofstream create(path_, std::ios::binary | std::ios::app);
    if (!create) {
      throw std::runtime_error("Failed to create WAL file");
    }
    create.close();
    if (!create) {
      throw std::runtime_error("Failed to close newly created WAL file");
    }
    detail::syncFileToStorage(path_);
    detail::syncDirectoryToStorage(path_.parent_path());
  }
  stream_.open(path_, std::ios::binary | std::ios::in | (readOnly_ ? std::ios::openmode{} : std::ios::out));
  if (!stream_) {
    throw std::runtime_error("Failed to open existing WAL file without truncation");
  }
}

void WriteAheadLog::close() {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) {
    if (!readOnly_) {
      stream_.flush();
      if (!stream_) throw std::runtime_error("Failed to flush WAL before close");
      detail::syncFileToStorage(path_);
    }
    stream_.close();
    if (stream_.fail()) throw std::runtime_error("Failed to close WAL file");
  }
}

void WriteAheadLog::append(const LogRecord& record) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot append to a read-only WAL");
  if (incompleteTailOffset_) throw std::runtime_error("WAL has an incomplete tail");
  const auto bytes = record.serialize();
  stream_.seekp(0, std::ios::end);
  if (!stream_) throw std::runtime_error("Failed to seek to WAL end");
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
}

std::vector<LogRecord> WriteAheadLog::readAll() {
  return readAll({}, nullptr, true);
}

std::vector<LogRecord> WriteAheadLog::readAll(
    const std::function<void(const LogRecord&)>& validate,
    const std::optional<WalIdentity>* expectedIdentity, bool repair) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  stream_.flush();
  if (!stream_) {
    throw std::runtime_error("Failed to flush WAL before replay");
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
  auto snapshotLsnSeen = !identity || identity->snapshotLsn == 0;
  std::uint64_t previousLsn = 0;
  std::uint64_t transactionHighWater = 0;
  const auto noteTail = [&] {
    if (!snapshotLsnSeen) throw std::runtime_error("WAL does not reach the snapshot LSN");
    incompleteTailOffset_ = validBytes;
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
    validBytes += header.size() + payload.size();
  }
  if (!snapshotLsnSeen) throw std::runtime_error("WAL does not reach the snapshot LSN");
  stream_.clear();
  return records;
}

void WriteAheadLog::flush() {
  std::lock_guard lock(mutex_);
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
  ensureOpen();
  const auto records = readAll();
  if (hasIdentity_) throw std::logic_error("WAL reclamation is disabled for bound databases");

  auto tempPath = path_;
  tempPath += ".rewrite.tmp";
  std::error_code removeError;
  std::filesystem::remove(tempPath, removeError);

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
}

void WriteAheadLog::repairPendingTail() {
  std::lock_guard lock(mutex_);
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
