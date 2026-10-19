#include "storage/Page.hpp"

#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace zenthrildb {

Page::Page(PageSize pageSize)
  : pageSize_(pageSize) {
  if (pageSize_ != kDefaultPageSize) {
    throw std::invalid_argument("ZenthrilDB MVP only supports 8192-byte pages");
  }
}

std::span<Byte> Page::payload() noexcept {
  const auto size = header_.version == kFormatVersionV2 ? kV2PayloadSize : kV1PayloadSize;
  return std::span<Byte>(data_).subspan(PageHeader::serializedSize(), size);
}

std::span<const Byte> Page::payload() const noexcept {
  const auto size = header_.version == kFormatVersionV2 ? kV2PayloadSize : kV1PayloadSize;
  return std::span<const Byte>(data_).subspan(PageHeader::serializedSize(), size);
}

std::uint64_t Page::pageLsn() const {
  if (header_.version != kFormatVersionV2) {
    throw std::logic_error("Page LSN is only present in page format v2");
  }
  return pageLsn_;
}

void Page::setPageLsn(std::uint64_t lsn) {
  if (header_.version != kFormatVersionV2) {
    throw std::logic_error("Page LSN is only present in page format v2");
  }
  pageLsn_ = lsn;
}

void Page::clear() noexcept {
  data_.fill(0);
  header_ = {};
  pageLsn_ = 0;
}

std::array<Byte, kDefaultPageSize> Page::serialize() const {
  const auto payloadCapacity = header_.version == kFormatVersionV1
    ? kV1PayloadSize
    : header_.version == kFormatVersionV2
      ? kV2PayloadSize
      : 0;
  if (payloadCapacity == 0) {
    throw std::invalid_argument("Unsupported page format version");
  }
  if (header_.version == kFormatVersionV2 && header_.usedBytes > payloadCapacity) {
    throw std::invalid_argument("Page used bytes exceed format payload capacity");
  }

  auto output = std::array<Byte, kDefaultPageSize>{};
  auto header = header_;
  header.checksum = 0;
  const auto headerBytes = header.serialize();
  std::copy(headerBytes.begin(), headerBytes.end(), output.begin());
  std::copy(data_.begin() + static_cast<std::ptrdiff_t>(PageHeader::serializedSize()),
            data_.begin() + static_cast<std::ptrdiff_t>(PageHeader::serializedSize() + payloadCapacity),
            output.begin() + static_cast<std::ptrdiff_t>(PageHeader::serializedSize()));

  if (header_.version == kFormatVersionV2) {
    std::size_t offset = kV2PageLsnOffset;
    binary::writeLittleEndian<std::uint64_t>(output, offset, pageLsn_);
  }

  const auto checksum = CRCManager::compute(output);
  std::size_t checksumOffset = sizeof(PageId) + sizeof(std::uint32_t);
  binary::writeLittleEndian<std::uint32_t>(output, checksumOffset, checksum);
  return output;
}

void Page::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != pageSize_) {
    throw std::invalid_argument("Invalid page size during deserialization");
  }
  std::copy(bytes.begin(), bytes.end(), data_.begin());
  header_.deserialize(std::span<const Byte>(data_.data(), PageHeader::serializedSize()));

  const auto payloadCapacity = header_.version == kFormatVersionV1
    ? kV1PayloadSize
    : header_.version == kFormatVersionV2
      ? kV2PayloadSize
      : 0;
  if (payloadCapacity == 0) {
    throw std::invalid_argument("Unsupported page format version");
  }
  if (header_.version == kFormatVersionV2 && header_.usedBytes > payloadCapacity) {
    throw std::invalid_argument("Page used bytes exceed format payload capacity");
  }

  if (header_.version == kFormatVersionV2) {
    std::size_t offset = kV2PageLsnOffset;
    pageLsn_ = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  } else {
    pageLsn_ = 0;
  }

  std::array<Byte, kDefaultPageSize> checksumBytes{};
  std::copy(bytes.begin(), bytes.end(), checksumBytes.begin());
  const auto expectedChecksum = header_.checksum;
  std::fill_n(checksumBytes.begin() + static_cast<std::ptrdiff_t>(sizeof(PageId) + sizeof(std::uint32_t)),
              sizeof(std::uint32_t), Byte{0});
  if (CRCManager::compute(checksumBytes) != expectedChecksum) {
    throw std::invalid_argument("Page checksum verification failed");
  }
}

} // namespace zenthrildb
