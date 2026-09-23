#pragma once

#include "core/Types.hpp"

#include <array>
#include <chrono>
#include <span>
#include <string>

namespace zenthrildb {

struct DatabaseHeader {
  static constexpr std::size_t kUuidSize = 16;
  static constexpr std::size_t kSerializedSize =
    sizeof(std::uint32_t) * 3 + sizeof(std::uint64_t) * 2 + kUuidSize + sizeof(std::uint32_t);

  std::uint32_t magic{ kDatabaseMagic };
  std::uint32_t version{ kCurrentDatabaseVersion };
  std::uint32_t pageSize{ kDefaultPageSize };
  std::uint64_t totalPages{0};
  std::uint64_t creationTimestampUnixNs{0};
  std::array<std::uint8_t, kUuidSize> databaseUuid{};
  std::uint32_t checksum{0};

  static constexpr std::size_t serializedSize() noexcept {
    return kSerializedSize;
  }

  [[nodiscard]] std::array<Byte, kSerializedSize> serialize() const noexcept;
  void deserialize(std::span<const Byte> bytes);
};

} // namespace zenthrildb
