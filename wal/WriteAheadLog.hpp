#pragma once

#include "wal/LogRecord.hpp"
#include "storage/WalIdentity.hpp"
#include "storage/StorageLimits.hpp"
#include "core/DatabaseFileLock.hpp"
#include <memory>

#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

namespace zenthrildb {

struct WalLimits {
  std::uintmax_t maxBytes{kMaxWalBytes};
  std::size_t maxRecords{100000};
};

class WriteAheadLog {
public:
  WriteAheadLog() = default;
  explicit WriteAheadLog(std::filesystem::path path, WalLimits limits = {});
  ~WriteAheadLog();

  void open(const std::filesystem::path& path);
  void close();
  void append(const LogRecord& record);
  [[nodiscard]] std::vector<LogRecord> readAll();
  void flush();
  void truncate(LogSequenceNumber inclusiveMaxLsn);
  [[nodiscard]] bool isOpen() const noexcept { return stream_.is_open(); }
  [[nodiscard]] bool wasCreated() const noexcept { return createdOnOpen_; }
  [[nodiscard]] bool hadIncompleteTail() const noexcept { return incompleteTailRepaired_; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  friend class LogManager;
  void open(const std::filesystem::path& path, bool allowCreate, bool readOnly = false,
            std::shared_ptr<DatabaseFileLock> ownership = {});
  [[nodiscard]] std::unique_lock<std::recursive_mutex> ownershipGuard() const;
  [[nodiscard]] std::vector<LogRecord> readAll(
    const std::function<void(const LogRecord&)>& validate,
    const std::optional<WalIdentity>* expectedIdentity, bool repair);
  void repairPendingTail();
  void ensureOpen() const;
  void truncateIncompleteTail(std::uintmax_t validBytes);

  mutable std::recursive_mutex mutex_;
  std::filesystem::path path_{};
  std::shared_ptr<DatabaseFileLock> databaseOwner_;
  std::shared_ptr<DatabaseFileLock> walOwner_;
  std::fstream stream_{};
  bool createdOnOpen_{false};
  bool incompleteTailRepaired_{false};
  std::optional<std::uintmax_t> incompleteTailOffset_;
  bool hasIdentity_{false};
  bool readOnly_{false};
  WalLimits limits_{};
  std::optional<std::size_t> recordCount_;
};

} // namespace zenthrildb
