#include "wal/LogManager.hpp"
#include "wal/LogPayload.hpp"
#include "storage/IoFaultInjector.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zenthrildb {

LogManager::LogManager(std::filesystem::path walPath)
  : LogManager(std::move(walPath), std::nullopt) {}

LogManager::LogManager(std::filesystem::path walPath, const std::array<Byte, DatabaseHeader::kUuidSize>& databaseUuid)
  : LogManager(std::move(walPath), std::optional{databaseUuid}) {}

LogManager::LogManager(std::filesystem::path walPath,
                       std::optional<std::array<Byte, DatabaseHeader::kUuidSize>> databaseUuid)
  : databaseUuid_(databaseUuid), wal_(std::move(walPath)) {
  const auto records = readValidated();
  if (!records.empty()) nextLsn_ = records.back().lsn().next();
}

LogManager::LogManager(std::filesystem::path walPath, const DatabaseHeader& header, std::optional<WalIdentity> identity,
                       bool readOnly)
  : databaseUuid_(header.databaseUuid), identity_(identity), paired_(true) {
  if (!std::filesystem::is_regular_file(walPath)) throw std::runtime_error("Database WAL is missing or is not a regular file");
  wal_.open(walPath, false, readOnly);
  const auto records = readValidated();
  if (!records.empty()) nextLsn_ = records.back().lsn().next();
}

std::vector<LogRecord> LogManager::readValidated() {
  LogSequenceNumber previous{0};
  return wal_.readAll([&](const LogRecord& record) {
    if (record.lsn().value() <= previous.value()) {
      throw std::runtime_error("Corrupted WAL: LSNs are not strictly increasing");
    }
    previous = record.lsn();
    if (record.lsn().value() == 0 || record.lsn().value() == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("WAL contains an invalid or exhausted LSN");
    }
    if (databaseUuid_ && record.type() == LogRecordType::DatabaseHeaderImageWritten) {
      const auto payload = LogPayloadCodec::decodeDatabaseHeaderImageWritten(record.payload());
      DatabaseHeader before;
      DatabaseHeader after;
      before.deserialize(payload.beforeImage);
      after.deserialize(payload.afterImage);
      if (before.databaseUuid != *databaseUuid_ || after.databaseUuid != *databaseUuid_) {
        throw std::runtime_error("WAL database UUID mismatch; refusing to recover this database");
      }
    }
  }, paired_ ? &identity_ : nullptr, !paired_);
}

LogSequenceNumber LogManager::append(LogRecordType type, std::span<const Byte> payload) {
  std::lock_guard lock(mutex_);
  if (type == LogRecordType::SafeCheckpoint) throw std::invalid_argument("Use the runtime checkpoint service");
  ensureWritable();
  std::vector<Byte> payloadBytes(payload.begin(), payload.end());
  const auto assignedLsn = nextLsn();
  try {
    wal_.append(LogRecord{assignedLsn, type, std::move(payloadBytes)});
  } catch (...) {
    poisoned_ = true;
    throw;
  }
  return assignedLsn;
}

LogSequenceNumber LogManager::publishCheckpoint(const SafeCheckpointPayload& payload) {
  std::lock_guard lock(mutex_);
  ensureWritable();
  if (!paired_ || !identity_ || payload.databaseUuid != identity_->databaseUuid ||
      payload.generation != identity_->generation) throw std::logic_error("Checkpoint identity is not paired");
  const auto lsn = nextLsn();
  try {
    wal_.append(LogRecord{lsn, LogRecordType::SafeCheckpoint, LogPayloadCodec::encodeSafeCheckpoint(payload)});
    IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointFrameBuffered);
    wal_.flush();
    IoFaultInjector::failIfArmed(IoFaultPoint::CheckpointDurable);
  } catch (...) {
    poisoned_ = true;
    throw;
  }
  return lsn;
}

void LogManager::flush() {
  std::lock_guard lock(mutex_);
  try {
    wal_.flush();
  } catch (...) {
    poisoned_ = true;
    throw;
  }
}

LogSequenceNumber LogManager::checkpoint() {
  return appendWithLsn(LogRecordType::Checkpoint, [](LogSequenceNumber) { return std::vector<Byte>{}; });
}

std::vector<LogRecord> LogManager::replay() {
  std::lock_guard lock(mutex_);
  return readValidated();
}

void LogManager::truncate(LogSequenceNumber inclusiveMaxLsn) {
  std::lock_guard lock(mutex_);
  ensureWritable();
  if (paired_ || wal_.hasIdentity_) throw std::logic_error("WAL reclamation is disabled for database journals");
  if (inclusiveMaxLsn.value() == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("Cannot truncate beyond the maximum WAL LSN");
  }
  if (nextLsn_.value() <= inclusiveMaxLsn.value()) {
    nextLsn_ = inclusiveMaxLsn.next();
  }
  const auto markerLsn = nextLsn();
  try {
    wal_.append(LogRecord{markerLsn, LogRecordType::Checkpoint, {}});
    wal_.flush();
    wal_.truncate(inclusiveMaxLsn);
  } catch (...) {
    poisoned_ = true;
    throw;
  }
}

RecoveryContext LogManager::prepareRecovery() {
  RecoveryContext context;
  for (auto& record : replay()) {
    context.addRecord(std::move(record));
  }
  return context;
}

LogSequenceNumber LogManager::createCheckpoint() {
  return checkpoint();
}

LogSequenceNumber LogManager::nextLsn() {
  if (nextLsn_.value() == 0 || nextLsn_.value() == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("WAL LSN space exhausted");
  }
  const auto current = nextLsn_;
  nextLsn_ = nextLsn_.next();
  return current;
}

void LogManager::ensureWritable() const {
  if (poisoned_) {
    throw std::runtime_error("WAL manager is unusable after a write or sync failure; reopen and recover it");
  }
  if (wal_.incompleteTailOffset_) throw std::runtime_error("WAL has an incomplete tail; recover the database before appending");
}

void LogManager::markFailed() {
  std::lock_guard lock(mutex_);
  poisoned_ = true;
}

} // namespace zenthrildb
