#include "storage/DatabaseHeader.hpp"

#include "storage/BinaryIO.hpp"

#include <stdexcept>

namespace zenthrildb {

std::array<Byte, DatabaseHeader::kSerializedSize> DatabaseHeader::serialize() const noexcept {
  std::array<Byte, kSerializedSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, magic);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, version);
  binary::writeLittleEndian<PageSize>(bytes, offset, pageSize);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, totalPages);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, creationTimestampUnixNs);
  binary::writeBytes(bytes, offset, databaseUuid);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, checksum);
  return bytes;
}

void DatabaseHeader::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != serializedSize()) {
    throw std::invalid_argument("Invalid database header size");
  }
  std::size_t offset = 0;
  magic = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  pageSize = binary::readLittleEndian<PageSize>(bytes, offset);
  totalPages = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  creationTimestampUnixNs = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  binary::readBytes(bytes, offset, databaseUuid);
  checksum = binary::readLittleEndian<std::uint32_t>(bytes, offset);
}

} // namespace zenthrildb
