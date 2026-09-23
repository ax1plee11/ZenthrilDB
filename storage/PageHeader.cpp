#include "storage/PageHeader.hpp"

#include "storage/BinaryIO.hpp"

#include <stdexcept>

namespace zenthrildb {

std::array<Byte, PageHeader::kSerializedSize> PageHeader::serialize() const noexcept {
  std::array<Byte, kSerializedSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<PageId>(bytes, offset, pageId);
  const auto pageTypeValue = static_cast<std::uint32_t>(pageType);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, pageTypeValue);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, checksum);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, usedBytes);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, version);
  return bytes;
}

void PageHeader::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != serializedSize()) {
    throw std::invalid_argument("Invalid page header size");
  }
  std::size_t offset = 0;
  pageId = binary::readLittleEndian<PageId>(bytes, offset);
  const auto pageTypeValue = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  pageType = static_cast<PageType>(pageTypeValue);
  checksum = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  usedBytes = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
}

} // namespace zenthrildb
