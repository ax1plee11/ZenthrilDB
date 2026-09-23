#include "wal/LogManager.hpp"

#include <algorithm>
#include <utility>

namespace zenthrildb {

LogManager::LogManager(std::filesystem::path walPath)
  : wal_(std::move(walPath)) {
  const auto records = wal_.readAll();
  for (const auto& record : records) {
    if (!(record.lsn() < nextLsn_)) {
      nextLsn_ = record.lsn().next();
    }
  }
}

LogSequenceNumber LogManager::append(LogRecordType type, std::span<const Byte> payload) {
  std::lock_guard lock(mutex_);
  std::vector<Byte> payloadBytes(payload.begin(), payload.end());
  const auto assignedLsn = nextLsn();
  wal_.append(LogRecord{assignedLsn, type, std::move(payloadBytes)});
  return assignedLsn;
}

void LogManager::flush() {
  std::lock_guard lock(mutex_);
  wal_.flush();
}

LogSequenceNumber LogManager::checkpoint() {
  return append(LogRecordType::Checkpoint);
}

std::vector<LogRecord> LogManager::replay() {
  std::lock_guard lock(mutex_);
  return wal_.readAll();
}

void LogManager::truncate(LogSequenceNumber inclusiveMaxLsn) {
  std::lock_guard lock(mutex_);
  wal_.truncate(inclusiveMaxLsn);
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
  const auto current = nextLsn_;
  nextLsn_ = nextLsn_.next();
  return current;
}

} // namespace zenthrildb
