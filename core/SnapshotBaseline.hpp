#pragma once

#include "core/FileManager.hpp"
#include "storage/SnapshotManifest.hpp"

namespace zenthrildb {

class RecoveryContext;

class SnapshotBaseline final {
public:
  explicit SnapshotBaseline(const std::filesystem::path& database);
  [[nodiscard]] const SnapshotManifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] const DatabaseHeader& header() const noexcept { return baseline_.header(); }
  [[nodiscard]] Page readPage(PageId id) { return baseline_.readPage(id); }
  void validateWal(const std::filesystem::path& path, const RecoveryContext& context) const;
  static void validatePages(FileManager& files, std::uint64_t lastLsn);
  [[nodiscard]] static bool existsFor(const std::filesystem::path& database);

private:
  SnapshotManifest manifest_;
  FileManager baseline_;
};

} // namespace zenthrildb
