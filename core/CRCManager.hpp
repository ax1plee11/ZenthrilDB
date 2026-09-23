#pragma once

#include "core/Types.hpp"

#include <cstddef>
#include <span>

namespace zenthrildb {

class CRCManager {
public:
  [[nodiscard]] static std::uint32_t compute(std::span<const Byte> bytes) noexcept;
  [[nodiscard]] static bool verify(std::span<const Byte> bytes, std::uint32_t expectedChecksum) noexcept;
};

} // namespace zenthrildb
