#pragma once

#include "storage/DatabaseHeader.hpp"
#include <filesystem>
#include <stdexcept>

namespace zenthrildb {
inline constexpr std::uint64_t kMaxDatabasePages = 131072; // 1 GiB of pages.
inline constexpr std::uintmax_t kMaxDatabaseBytes =
  DatabaseHeader::kSerializedSize + kMaxDatabasePages * kDefaultPageSize;
inline constexpr std::uintmax_t kMaxWalBytes = 256ull * 1024 * 1024;

inline void checkStorageSize(const std::filesystem::path& path, std::uintmax_t maximum) {
  if (std::filesystem::exists(path) && std::filesystem::file_size(path) > maximum) {
    throw std::length_error("Storage file exceeds the supported resource limit");
  }
}
} // namespace zenthrildb
