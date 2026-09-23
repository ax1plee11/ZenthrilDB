#include "wal/WriteAheadLog.hpp"

#include "storage/BinaryIO.hpp"

#include <array>
#include <stdexcept>

namespace zenthrildb {

WriteAheadLog::WriteAheadLog(std::filesystem::path path) {
  open(path);
}

WriteAheadLog::~WriteAheadLog() {
  close();
}

void WriteAheadLog::open(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) {
    stream_.flush();
    stream_.close();
  }
  path_ = path;
  stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
  if (!stream_) {
    stream_.clear();
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  }
  if (!stream_) {
    throw std::runtime_error("Failed to open WAL file");
  }
}

void WriteAheadLog::close() {
  std::lock_guard lock(mutex_);
  if (stream_.is_open()) {
    stream_.flush();
    stream_.close();
  }
}

void WriteAheadLog::append(const LogRecord& record) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  const auto bytes = record.serialize();
  stream_.seekp(0, std::ios::end);
  stream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!stream_) {
    throw std::runtime_error("Failed to append WAL record");
  }
}

std::vector<LogRecord> WriteAheadLog::readAll() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  stream_.flush();
  stream_.clear();
  stream_.seekg(0, std::ios::beg);

  std::vector<LogRecord> records;
  while (true) {
    std::array<Byte, LogRecord::kHeaderSize> header{};
    stream_.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto bytesRead = stream_.gcount();
    if (bytesRead == 0) {
      break;
    }
    if (bytesRead != static_cast<std::streamsize>(header.size())) {
      throw std::runtime_error("Corrupted WAL: partial record header");
    }

    std::uint32_t payloadSize{};
    constexpr std::size_t payloadSizeOffset = sizeof(std::uint32_t) * 3 + sizeof(std::uint64_t) * 2;
    auto payloadSizeReadOffset = payloadSizeOffset;
    payloadSize = binary::readLittleEndian<std::uint32_t>(header, payloadSizeReadOffset);
    if (payloadSize > LogRecord::kMaxPayloadSize) {
      throw std::runtime_error("Corrupted WAL: payload exceeds maximum size");
    }
    std::vector<Byte> payload(payloadSize);
    if (payloadSize > 0) {
      stream_.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
      if (stream_.gcount() != static_cast<std::streamsize>(payload.size())) {
        throw std::runtime_error("Corrupted WAL: partial record payload");
      }
    }
    records.push_back(LogRecord::deserializeHeaderAndPayload(header, payload));
  }
  stream_.clear();
  return records;
}

void WriteAheadLog::flush() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  stream_.flush();
}

void WriteAheadLog::truncate(LogSequenceNumber inclusiveMaxLsn) {
  const auto records = readAll();
  std::lock_guard lock(mutex_);
  ensureOpen();
  stream_.close();
  stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!stream_) {
    throw std::runtime_error("Failed to truncate WAL file");
  }
  for (const auto& record : records) {
    if (record.lsn().value() > inclusiveMaxLsn.value()) {
      const auto bytes = record.serialize();
      stream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!stream_) {
        throw std::runtime_error("Failed to rewrite WAL during truncation");
      }
    }
  }
}

void WriteAheadLog::ensureOpen() const {
  if (!stream_.is_open()) {
    throw std::runtime_error("WAL file is not open");
  }
}

} // namespace zenthrildb
