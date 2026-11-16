#pragma once

#include "storage/WalIdentity.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace zenthrildb {

struct SnapshotManifest {
  static constexpr std::uint32_t kMagic = 0x5342445Au; // ZDBS
  static constexpr std::uint32_t kVersion = 1;
  static constexpr std::size_t kSize =
    6 * sizeof(std::uint32_t) + WalIdentity::kSize + 4 * sizeof(std::uint64_t);

  WalIdentity identity;
  std::uint64_t transactionHighWater{};
  std::uint64_t pageCount{};
  std::uint64_t databaseBytes{};
  std::uint64_t walPrefixBytes{};
  std::uint32_t databaseCrc{};
  std::uint32_t walPrefixCrc{};

  bool operator==(const SnapshotManifest&) const = default;
  [[nodiscard]] std::array<Byte, kSize> serialize() const;
  [[nodiscard]] static SnapshotManifest deserialize(std::span<const Byte> bytes);
};

[[nodiscard]] std::filesystem::path snapshotManifestPath(const std::filesystem::path& database);
[[nodiscard]] std::filesystem::path snapshotBaselinePath(const std::filesystem::path& database);
[[nodiscard]] SnapshotManifest readSnapshotManifest(const std::filesystem::path& path);

// The caller owns staging and the durability barriers before publication.
void writeSnapshotManifest(const std::filesystem::path& path, const SnapshotManifest& manifest);

// Calculates CRC32 over exactly the requested prefix. A short file is an error.
[[nodiscard]] std::uint32_t snapshotFileCrc(const std::filesystem::path& path, std::uint64_t bytes);

} // namespace zenthrildb
