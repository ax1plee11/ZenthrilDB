#pragma once

#include "storage/DatabaseWalPath.hpp"
#include "storage/SnapshotManifest.hpp"

#include <filesystem>

inline void removeTestDatabaseArtifacts(const std::filesystem::path& databasePath) {
  std::error_code ignored;
  std::filesystem::remove(databasePath, ignored);
  std::filesystem::remove(zenthrildb::databaseWalPath(databasePath), ignored);
  std::filesystem::remove(zenthrildb::snapshotBaselinePath(databasePath), ignored);
  std::filesystem::remove(zenthrildb::snapshotManifestPath(databasePath), ignored);
  auto lockPath = databasePath;
  lockPath += ".lock";
  std::filesystem::remove(lockPath, ignored);
  auto walLock = zenthrildb::databaseWalPath(databasePath); walLock += ".lock";
  std::filesystem::remove(walLock, ignored);
}
