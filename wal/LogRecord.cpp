#include "wal/LogRecord.hpp"

#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"

#include <chrono>
#include <stdexcept>

namespace zenthrildb {

LogRecord::LogRecord(LogSequenceNumber lsn, LogRecordType type, std::vector<Byte> payload)
  : lsn_(lsn), type_(type), payload_(std::move(payload)) {
  if (payload_.size() > kMaxPayloadSize) {
    throw std::invalid_argument("WAL record payload exceeds maximum size");
  }
  timestampUnixNs_ = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  crc32_ = computeChecksum();
}

std::vector<Byte> LogRecord::serialize() const {
  return serializeWithChecksum(crc32_);
}

LogRecord LogRecord::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() < kHeaderSize) {
    throw std::invalid_argument("WAL record is smaller than header");
  }
  std::size_t payloadSizeOffset = sizeof(std::uint32_t) * 3 + sizeof(std::uint64_t) * 2;
  const auto payloadSize = binary::readLittleEndian<std::uint32_t>(bytes, payloadSizeOffset);
  if (payloadSize > kMaxPayloadSize) {
    throw std::invalid_argument("WAL record payload exceeds maximum size");
  }
  if (bytes.size() != kHeaderSize + payloadSize) {
    throw std::invalid_argument("WAL record size does not match payload size");
  }
  return deserializeHeaderAndPayload(bytes.subspan(0, kHeaderSize), bytes.subspan(kHeaderSize));
}

LogRecord LogRecord::deserializeHeaderAndPayload(std::span<const Byte> header, std::span<const Byte> payload) {
  if (header.size() != kHeaderSize) {
    throw std::invalid_argument("Invalid WAL header size");
  }

  std::size_t offset = 0;
  std::uint32_t magic{};
  std::uint32_t version{};
  std::uint32_t typeValue{};
  std::uint64_t lsnValue{};
  std::uint64_t timestamp{};
  std::uint32_t payloadSize{};
  std::uint32_t crc{};

  magic = binary::readLittleEndian<std::uint32_t>(header, offset);
  version = binary::readLittleEndian<std::uint32_t>(header, offset);
  typeValue = binary::readLittleEndian<std::uint32_t>(header, offset);
  lsnValue = binary::readLittleEndian<std::uint64_t>(header, offset);
  timestamp = binary::readLittleEndian<std::uint64_t>(header, offset);
  payloadSize = binary::readLittleEndian<std::uint32_t>(header, offset);
  crc = binary::readLittleEndian<std::uint32_t>(header, offset);

  if (magic != kMagic) {
    throw std::invalid_argument("Invalid WAL record magic");
  }
  if (version != kFormatVersion) {
    throw std::invalid_argument("Unsupported WAL record version");
  }
  if (payload.size() != payloadSize) {
    throw std::invalid_argument("Invalid WAL payload size");
  }
  if (payloadSize > kMaxPayloadSize) {
    throw std::invalid_argument("WAL record payload exceeds maximum size");
  }

  LogRecord record;
  record.version_ = version;
  record.type_ = static_cast<LogRecordType>(typeValue);
  record.lsn_ = LogSequenceNumber{lsnValue};
  record.timestampUnixNs_ = timestamp;
  record.payload_.assign(payload.begin(), payload.end());
  record.crc32_ = crc;

  if (record.computeChecksum() != crc) {
    throw std::invalid_argument("WAL record CRC validation failed");
  }
  return record;
}

std::vector<Byte> LogRecord::serializeWithChecksum(std::uint32_t checksum) const {
  std::vector<Byte> bytes;
  if (payload_.size() > kMaxPayloadSize) {
    throw std::invalid_argument("WAL record payload exceeds maximum size");
  }
  bytes.reserve(kHeaderSize + payload_.size());
  binary::appendLittleEndian<std::uint32_t>(bytes, kMagic);
  binary::appendLittleEndian<std::uint32_t>(bytes, version_);
  const auto typeValue = static_cast<std::uint32_t>(type_);
  binary::appendLittleEndian<std::uint32_t>(bytes, typeValue);
  const auto lsnValue = lsn_.value();
  binary::appendLittleEndian<std::uint64_t>(bytes, lsnValue);
  binary::appendLittleEndian<std::uint64_t>(bytes, timestampUnixNs_);
  const auto payloadSize = static_cast<std::uint32_t>(payload_.size());
  binary::appendLittleEndian<std::uint32_t>(bytes, payloadSize);
  binary::appendLittleEndian<std::uint32_t>(bytes, checksum);
  bytes.insert(bytes.end(), payload_.begin(), payload_.end());
  return bytes;
}

std::uint32_t LogRecord::computeChecksum() const {
  const auto bytes = serializeWithChecksum(0);
  return CRCManager::compute(std::span<const Byte>(bytes.data(), bytes.size()));
}

} // namespace zenthrildb
