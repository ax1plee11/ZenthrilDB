#pragma once

#include "storage/DatabaseHeader.hpp"
#include "storage/Page.hpp"

#include <iosfwd>
#include <optional>

namespace zenthrildb {

struct WalIdentity {
  static constexpr std::uint32_t kMagic = 0x4942445Au; // ZDBI
  static constexpr std::uint32_t kVersion = 1;
  static constexpr std::uint32_t kBaselineRequiredVersion = 2;
  static constexpr std::uint32_t kReducedHistoryVersion = 3;
  static constexpr std::size_t kSize = 60;

  std::array<Byte, 16> generation{};
  std::array<Byte, DatabaseHeader::kUuidSize> databaseUuid{};
  std::uint64_t snapshotLsn{};
  bool baselineRequired{false};
  bool reducedHistory{false};

  bool operator==(const WalIdentity&) const = default;
  [[nodiscard]] std::array<Byte, kSize> serialize() const;
  [[nodiscard]] static WalIdentity deserialize(std::span<const Byte> bytes);
  [[nodiscard]] static WalIdentity fresh(const DatabaseHeader& header, std::uint64_t snapshotLsn);
  [[nodiscard]] static std::array<Byte, 16> newGeneration();
  [[nodiscard]] Page toPage() const;
};

// Returns nullopt for a legacy stream and positions the stream at its first frame.
[[nodiscard]] std::optional<WalIdentity> readWalIdentity(std::istream& stream);

} // namespace zenthrildb
