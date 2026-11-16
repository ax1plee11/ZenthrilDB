#pragma once

#include "storage/DatabaseHeader.hpp"
#include "storage/Page.hpp"
#include "storage/WalIdentity.hpp"
#include "core/DatabaseFileLock.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <memory>

namespace zenthrildb {

class RecoveryCoordinator;
class SnapshotBaseline;
class DatabaseRuntime;

class PageNotFoundError final : public std::runtime_error {
public:
  PageNotFoundError() : std::runtime_error("Database page is beyond end of file") {}
};

class FileManager {
public:
  FileManager() = default;
  ~FileManager();

  void createDatabase(const std::filesystem::path& path);
  void createDatabase(const std::filesystem::path& path, bool bindWal);
  void openDatabase(const std::filesystem::path& path);
  void closeDatabase();

  [[nodiscard]] Page readPage(PageId pageId);
  void writePage(const Page& page);
  void flush();
  [[nodiscard]] PageId reservePageId();
  [[nodiscard]] std::uint64_t physicalPageCount() const;
  void verifyPhysicalLayout() const;
  void restoreHeaderImage(const std::array<Byte, DatabaseHeader::serializedSize()>& image, bool truncateTail);
  void markFailed() noexcept { failed_ = true; }

  [[nodiscard]] const DatabaseHeader& header() const noexcept { return header_; }
  [[nodiscard]] const std::optional<WalIdentity>& walIdentity() const noexcept { return walIdentity_; }
  [[nodiscard]] std::optional<std::uint64_t> recoverySnapshotBoundary() const noexcept { return recoverySnapshotBoundary_; }
  [[nodiscard]] bool isOpen() const noexcept { return fileStream_.is_open(); }

private:
  friend class RecoveryCoordinator;
  friend class SnapshotBaseline;
  friend class DatabaseRuntime;
  void openReadOnly(const std::filesystem::path& path);
  struct PhysicalLayout {
    std::uint64_t completePages;
    bool hasPartialPage;
  };
  [[nodiscard]] PhysicalLayout physicalLayout() const;
  void ensureOpen() const;
  void writeHeader();
  void readHeader();
  void readIdentity();
  static std::streamoff pageOffset(PageId pageId) noexcept;

  mutable std::recursive_mutex mutex_;
  std::fstream fileStream_;
  std::filesystem::path path_;
  std::unique_ptr<DatabaseFileLock> processLock_;
  DatabaseHeader header_{};
  std::optional<WalIdentity> walIdentity_;
  std::optional<std::uint64_t> recoverySnapshotBoundary_;
  bool failed_{false};
  bool readOnly_{false};
};

} // namespace zenthrildb
