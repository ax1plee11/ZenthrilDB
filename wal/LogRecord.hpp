#pragma once

#include "core/Types.hpp"
#include "wal/LogSequenceNumber.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace zenthrildb {

enum class LogRecordType : std::uint32_t {
  Unknown = 0,
  DatabaseCreated = 1,
  DatabaseOpened = 2,
  PageAllocated = 3,
  PageFreed = 4,
  PageWritten = 5,
  MetadataUpdated = 6,
  Checkpoint = 7,
  RecoveryMarker = 8,
  TransactionStarted = 9,
  TransactionCommitted = 10,
  TransactionRolledBack = 11,
};

class LogRecord {
public:
  static constexpr std::uint32_t kMagic = 0x5A57414Cu; // ZWAL
  static constexpr std::uint32_t kFormatVersion = 1;
  static constexpr std::uint32_t kMaxPayloadSize = 64u * 1024u * 1024u;
  static constexpr std::size_t kHeaderSize =
    sizeof(std::uint32_t) * 5 + sizeof(std::uint64_t) * 2;

  LogRecord() = default;
  LogRecord(LogSequenceNumber lsn, LogRecordType type, std::vector<Byte> payload);

  [[nodiscard]] LogSequenceNumber lsn() const noexcept { return lsn_; }
  [[nodiscard]] LogRecordType type() const noexcept { return type_; }
  [[nodiscard]] std::uint64_t timestampUnixNs() const noexcept { return timestampUnixNs_; }
  [[nodiscard]] std::uint32_t crc32() const noexcept { return crc32_; }
  [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
  [[nodiscard]] const std::vector<Byte>& payload() const noexcept { return payload_; }

  [[nodiscard]] std::vector<Byte> serialize() const;
  static LogRecord deserialize(std::span<const Byte> bytes);
  static LogRecord deserializeHeaderAndPayload(std::span<const Byte> header, std::span<const Byte> payload);

private:
  [[nodiscard]] std::vector<Byte> serializeWithChecksum(std::uint32_t checksum) const;
  [[nodiscard]] std::uint32_t computeChecksum() const;

  std::uint32_t version_{kFormatVersion};
  LogSequenceNumber lsn_{};
  LogRecordType type_{LogRecordType::Unknown};
  std::uint64_t timestampUnixNs_{0};
  std::vector<Byte> payload_{};
  std::uint32_t crc32_{0};
};

} // namespace zenthrildb
