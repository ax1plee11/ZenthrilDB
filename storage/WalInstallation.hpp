#pragma once
#include "storage/WalIdentity.hpp"
#include <filesystem>
#include <vector>

namespace zenthrildb {
// Immutable commit selector. No timestamp-based selection or fallback is allowed.
struct WalInstallation {
  WalIdentity source;
  WalIdentity target;
  std::uint64_t sourceDataBytes{};
  std::uint64_t sourceWalBytes{};
  bool sourceHasBaseline{};
  std::filesystem::path targetPath;
  [[nodiscard]] std::vector<Byte> serialize() const;
  [[nodiscard]] static WalInstallation deserialize(std::span<const Byte> bytes);
};
[[nodiscard]] std::filesystem::path walInstallationPath(const std::filesystem::path& database);
[[nodiscard]] WalInstallation readWalInstallation(const std::filesystem::path& database);
void publishWalInstallation(const std::filesystem::path& database, const WalInstallation& state);
}
