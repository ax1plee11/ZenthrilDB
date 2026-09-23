#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace zenthrildb {

enum class PageType : std::uint32_t {
  Metadata = 1,
  Table = 2,
  Index = 3,
  Free = 4,
  Overflow = 5,
};

struct PageHeader {
  static constexpr std::size_t kSerializedSize = sizeof(PageId) + sizeof(std::uint32_t) * 4;

  PageId pageId{0};
  PageType pageType{PageType::Free};
  std::uint32_t checksum{0};
  std::uint32_t usedBytes{0};
  std::uint32_t version{1};

  static constexpr std::size_t serializedSize() noexcept {
    return kSerializedSize;
  }

  [[nodiscard]] std::array<Byte, kSerializedSize> serialize() const noexcept;
  void deserialize(std::span<const Byte> bytes);
};

} // namespace zenthrildb
