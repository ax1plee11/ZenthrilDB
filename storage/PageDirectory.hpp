#pragma once

#include "core/Types.hpp"
#include "storage/PageHeader.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace zenthrildb {

enum class PageStatus : std::uint32_t {
  Free = 0,
  Reserved = 1,
  InUse = 2,
  Deleted = 3,
};

struct PageDirectoryEntry {
  static constexpr std::size_t kMaxOwnerLength = 64;
  static constexpr std::size_t kSerializedSize =
    sizeof(PageId) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
    sizeof(std::uint32_t) + sizeof(std::uint32_t) + kMaxOwnerLength;

  PageId pageId{0};
  PageType pageType{PageType::Free};
  std::string ownerTable{};
  std::uint64_t creationTimestampUnixNs{0};
  PageStatus status{PageStatus::Free};

  static constexpr std::size_t serializedSize() noexcept {
    return kSerializedSize;
  }

  [[nodiscard]] std::array<Byte, kSerializedSize> serialize() const;
  void deserialize(std::span<const Byte> bytes);
};

class PageDirectory {
public:
  static constexpr std::uint32_t kMetadataVersion = 1;

  void registerPage(const PageDirectoryEntry& entry);
  void unregisterPage(PageId pageId);
  [[nodiscard]] const PageDirectoryEntry* get(PageId pageId) const;
  [[nodiscard]] std::vector<PageDirectoryEntry> entries() const;
  [[nodiscard]] std::array<Byte, kDefaultPageSize - PageHeader::serializedSize()> serialize() const;
  void deserialize(std::span<const Byte> bytes);

private:
  std::unordered_map<PageId, PageDirectoryEntry> entries_;
};

} // namespace zenthrildb
