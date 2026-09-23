#include "wal/LogPayload.hpp"

#include "storage/BinaryIO.hpp"

#include <stdexcept>

namespace zenthrildb {

namespace {

void appendVersion(std::vector<Byte>& bytes) {
  binary::appendLittleEndian<std::uint32_t>(bytes, LogPayloadCodec::kPayloadFormatVersion);
}

void validateVersion(std::span<const Byte> bytes, std::size_t& offset) {
  const auto version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (version != LogPayloadCodec::kPayloadFormatVersion) {
    throw std::invalid_argument("Unsupported WAL payload version");
  }
}

void ensureFullyRead(std::span<const Byte> bytes, std::size_t offset) {
  if (offset != bytes.size()) {
    throw std::invalid_argument("WAL payload contains trailing bytes");
  }
}

void ensurePageId(PageId pageId) {
  if (pageId == 0) {
    throw std::invalid_argument("WAL payload contains an invalid page id");
  }
}

PageType decodePageType(std::uint32_t value) {
  switch (static_cast<PageType>(value)) {
    case PageType::Metadata:
    case PageType::Table:
    case PageType::Index:
    case PageType::Free:
    case PageType::Overflow:
      return static_cast<PageType>(value);
    default:
      throw std::invalid_argument("WAL payload contains an unknown page type");
  }
}

MetadataPayloadKind decodeMetadataKind(std::uint32_t value) {
  switch (static_cast<MetadataPayloadKind>(value)) {
    case MetadataPayloadKind::DatabaseMetadata:
    case MetadataPayloadKind::PageDirectory:
    case MetadataPayloadKind::FreePageList:
    case MetadataPayloadKind::TableRegistry:
      return static_cast<MetadataPayloadKind>(value);
    default:
      throw std::invalid_argument("WAL payload contains an unknown metadata kind");
  }
}

} // namespace

std::vector<Byte> LogPayloadCodec::encodePageAllocation(const PageAllocationPayload& payload) {
  ensurePageId(payload.pageId);
  std::vector<Byte> bytes;
  bytes.reserve(sizeof(std::uint32_t) * 3 + sizeof(std::uint64_t));
  appendVersion(bytes);
  binary::appendLittleEndian<PageId>(bytes, payload.pageId);
  binary::appendLittleEndian<std::uint32_t>(bytes, static_cast<std::uint32_t>(payload.pageType));
  binary::appendLittleEndian<std::uint64_t>(bytes, payload.ownerTableId);
  return bytes;
}

PageAllocationPayload LogPayloadCodec::decodePageAllocation(std::span<const Byte> bytes) {
  std::size_t offset = 0;
  validateVersion(bytes, offset);
  PageAllocationPayload payload;
  payload.pageId = binary::readLittleEndian<PageId>(bytes, offset);
  ensurePageId(payload.pageId);
  payload.pageType = decodePageType(binary::readLittleEndian<std::uint32_t>(bytes, offset));
  payload.ownerTableId = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  ensureFullyRead(bytes, offset);
  return payload;
}

std::vector<Byte> LogPayloadCodec::encodePageFree(const PageFreePayload& payload) {
  ensurePageId(payload.pageId);
  std::vector<Byte> bytes;
  bytes.reserve(sizeof(std::uint32_t) * 2 + sizeof(std::uint64_t));
  appendVersion(bytes);
  binary::appendLittleEndian<PageId>(bytes, payload.pageId);
  binary::appendLittleEndian<std::uint64_t>(bytes, payload.ownerTableId);
  return bytes;
}

PageFreePayload LogPayloadCodec::decodePageFree(std::span<const Byte> bytes) {
  std::size_t offset = 0;
  validateVersion(bytes, offset);
  PageFreePayload payload;
  payload.pageId = binary::readLittleEndian<PageId>(bytes, offset);
  ensurePageId(payload.pageId);
  payload.ownerTableId = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  ensureFullyRead(bytes, offset);
  return payload;
}

std::vector<Byte> LogPayloadCodec::encodePageWritten(const PageWrittenPayload& payload) {
  ensurePageId(payload.pageId);
  std::vector<Byte> bytes;
  bytes.reserve(sizeof(std::uint32_t) * 6 + sizeof(std::uint64_t));
  appendVersion(bytes);
  binary::appendLittleEndian<PageId>(bytes, payload.pageId);
  binary::appendLittleEndian<std::uint32_t>(bytes, static_cast<std::uint32_t>(payload.pageType));
  binary::appendLittleEndian<std::uint32_t>(bytes, payload.pageChecksum);
  binary::appendLittleEndian<std::uint32_t>(bytes, payload.usedBytes);
  binary::appendLittleEndian<std::uint32_t>(bytes, payload.pageVersion);
  binary::appendLittleEndian<std::uint64_t>(bytes, payload.ownerTableId);
  return bytes;
}

PageWrittenPayload LogPayloadCodec::decodePageWritten(std::span<const Byte> bytes) {
  std::size_t offset = 0;
  validateVersion(bytes, offset);
  PageWrittenPayload payload;
  payload.pageId = binary::readLittleEndian<PageId>(bytes, offset);
  ensurePageId(payload.pageId);
  payload.pageType = decodePageType(binary::readLittleEndian<std::uint32_t>(bytes, offset));
  payload.pageChecksum = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  payload.usedBytes = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  payload.pageVersion = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  payload.ownerTableId = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  ensureFullyRead(bytes, offset);
  return payload;
}

std::vector<Byte> LogPayloadCodec::encodeMetadataUpdated(const MetadataUpdatedPayload& payload) {
  ensurePageId(payload.pageId);
  std::vector<Byte> bytes;
  bytes.reserve(sizeof(std::uint32_t) * 5);
  appendVersion(bytes);
  binary::appendLittleEndian<std::uint32_t>(bytes, static_cast<std::uint32_t>(payload.kind));
  binary::appendLittleEndian<PageId>(bytes, payload.pageId);
  binary::appendLittleEndian<std::uint32_t>(bytes, payload.metadataVersion);
  binary::appendLittleEndian<std::uint32_t>(bytes, payload.payloadChecksum);
  return bytes;
}

MetadataUpdatedPayload LogPayloadCodec::decodeMetadataUpdated(std::span<const Byte> bytes) {
  std::size_t offset = 0;
  validateVersion(bytes, offset);
  MetadataUpdatedPayload payload;
  payload.kind = decodeMetadataKind(binary::readLittleEndian<std::uint32_t>(bytes, offset));
  payload.pageId = binary::readLittleEndian<PageId>(bytes, offset);
  ensurePageId(payload.pageId);
  payload.metadataVersion = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  payload.payloadChecksum = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  ensureFullyRead(bytes, offset);
  return payload;
}

std::vector<Byte> LogPayloadCodec::encodeTransaction(const TransactionPayload& payload) {
  if (!payload.transactionId.valid()) {
    throw std::invalid_argument("WAL payload contains an invalid transaction id");
  }
  std::vector<Byte> bytes;
  bytes.reserve(sizeof(std::uint32_t) + sizeof(std::uint64_t));
  appendVersion(bytes);
  binary::appendLittleEndian<std::uint64_t>(bytes, payload.transactionId.value());
  return bytes;
}

TransactionPayload LogPayloadCodec::decodeTransaction(std::span<const Byte> bytes) {
  std::size_t offset = 0;
  validateVersion(bytes, offset);
  TransactionPayload payload;
  payload.transactionId = TransactionId{binary::readLittleEndian<std::uint64_t>(bytes, offset)};
  if (!payload.transactionId.valid()) {
    throw std::invalid_argument("WAL payload contains an invalid transaction id");
  }
  ensureFullyRead(bytes, offset);
  return payload;
}

} // namespace zenthrildb
