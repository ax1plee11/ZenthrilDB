#pragma once

#include <filesystem>

namespace zenthrildb {

[[nodiscard]] inline std::filesystem::path databaseWalPath(const std::filesystem::path& databasePath) {
  auto result = databasePath;
  result.replace_extension(".wal");
  return result;
}

} // namespace zenthrildb
