#include "core/CRCManager.hpp"

namespace zenthrildb {

static constexpr std::uint32_t kPolynomial = 0xEDB88320u;

std::uint32_t CRCManager::compute(std::span<const Byte> bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      const auto mask = static_cast<std::uint32_t>(-(static_cast<int>(crc & 1u)));
      crc = (crc >> 1) ^ (kPolynomial & mask);
    }
  }
  return ~crc;
}

bool CRCManager::verify(std::span<const Byte> bytes, std::uint32_t expectedChecksum) noexcept {
  return compute(bytes) == expectedChecksum;
}

} // namespace zenthrildb
