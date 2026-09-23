#include "storage/TableMetadata.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/BinaryFormat.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace zenthrildb {

TableMetadata::TableMetadata(std::uint64_t tableId, std::string name, PageId rootPageId)
  : tableId_(tableId), name_(std::move(name)), rootPageId_(rootPageId) {
  if (name_.size() > kMaxNameLength) {
    throw std::invalid_argument("Table name exceeds maximum length");
  }
  creationTimestampUnixNs_ = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

void TableMetadata::rename(std::string name) {
  if (name.size() > kMaxNameLength) {
    throw std::invalid_argument("Table name exceeds maximum length");
  }
  name_ = std::move(name);
  ++tableVersion_;
}

std::array<Byte, TableMetadata::kSerializedSize> TableMetadata::serialize() const noexcept {
  std::array<Byte, kSerializedSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, formatVersion_);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, tableId_);
  binary::writeLittleEndian<PageId>(bytes, offset, rootPageId_);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, creationTimestampUnixNs_);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, recordCount_);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, pageCount_);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, tableVersion_);
  const auto nameSize = static_cast<std::uint32_t>(name_.size());
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, nameSize);
  binary::writeStringBytes(bytes, offset, name_);
  return bytes;
}

void TableMetadata::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != kSerializedSize) {
    throw std::invalid_argument("Invalid table metadata size");
  }
  std::size_t offset = 0;
  formatVersion_ = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (formatVersion_ != format::kTableMetadataFormatVersion) {
    throw std::invalid_argument("Unsupported table metadata format version");
  }
  tableId_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  rootPageId_ = binary::readLittleEndian<PageId>(bytes, offset);
  creationTimestampUnixNs_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  recordCount_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  pageCount_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  tableVersion_ = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto nameSize = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (nameSize > kMaxNameLength || offset + nameSize > bytes.size()) {
    throw std::invalid_argument("Invalid table name encoding");
  }
  name_.assign(reinterpret_cast<const char*>(bytes.data() + offset), nameSize);
}

} // namespace zenthrildb
