#pragma once

#include <cstddef>
#include <cstdint>

namespace zenthrildb {

using Byte = std::uint8_t;
using PageId = std::uint32_t;
using PageSize = std::uint32_t;

inline constexpr PageSize kDefaultPageSize = 8192;
inline constexpr std::uint32_t kCurrentDatabaseVersion = 1;
inline constexpr std::uint32_t kDatabaseMagic = 0x5A444231; // ZDB1

// DatabaseHeader remains a file prefix for backward compatibility. Page 0 is
// reserved as a header mirror slot for future layout evolution.
inline constexpr PageId kHeaderMirrorPageId = 0;
inline constexpr PageId kDatabaseMetadataPageId = 1;
inline constexpr PageId kPageDirectoryPageId = 2;
inline constexpr PageId kFreePageListPageId = 3;
inline constexpr PageId kFirstUserPageId = 4;

} // namespace zenthrildb
