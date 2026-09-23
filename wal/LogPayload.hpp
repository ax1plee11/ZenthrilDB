#pragma once

#include "core/Types.hpp"
#include "storage/PageHeader.hpp"
#include "transaction/TransactionId.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace zenthrildb {

enum class MetadataPayloadKind : std::uint32_t {
  DatabaseMetadata = 1,
  PageDirectory = 2,
  FreePageList = 3,
  TableRegistry = 4,
};

struct PageAllocationPayload {
  PageId pageId{0};
  PageType pageType{PageType::Free};
  std::uint64_t ownerTableId{0};
};

struct PageFreePayload {
  PageId pageId{0};
  std::uint64_t ownerTableId{0};
};

struct PageWrittenPayload {
  PageId pageId{0};
  PageType pageType{PageType::Free};
  std::uint32_t pageChecksum{0};
  std::uint32_t usedBytes{0};
  std::uint32_t pageVersion{0};
  std::uint64_t ownerTableId{0};
};

struct MetadataUpdatedPayload {
  MetadataPayloadKind kind{MetadataPayloadKind::DatabaseMetadata};
  PageId pageId{0};
  std::uint32_t metadataVersion{0};
  std::uint32_t payloadChecksum{0};
};

struct TransactionPayload {
  TransactionId transactionId{};
};

class LogPayloadCodec {
public:
  static constexpr std::uint32_t kPayloadFormatVersion = 1;

  [[nodiscard]] static std::vector<Byte> encodePageAllocation(const PageAllocationPayload& payload);
  [[nodiscard]] static PageAllocationPayload decodePageAllocation(std::span<const Byte> bytes);

  [[nodiscard]] static std::vector<Byte> encodePageFree(const PageFreePayload& payload);
  [[nodiscard]] static PageFreePayload decodePageFree(std::span<const Byte> bytes);

  [[nodiscard]] static std::vector<Byte> encodePageWritten(const PageWrittenPayload& payload);
  [[nodiscard]] static PageWrittenPayload decodePageWritten(std::span<const Byte> bytes);

  [[nodiscard]] static std::vector<Byte> encodeMetadataUpdated(const MetadataUpdatedPayload& payload);
  [[nodiscard]] static MetadataUpdatedPayload decodeMetadataUpdated(std::span<const Byte> bytes);

  [[nodiscard]] static std::vector<Byte> encodeTransaction(const TransactionPayload& payload);
  [[nodiscard]] static TransactionPayload decodeTransaction(std::span<const Byte> bytes);
};

} // namespace zenthrildb
