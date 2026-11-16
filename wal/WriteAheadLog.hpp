#pragma once

#include "wal/LogRecord.hpp"
#include "storage/WalIdentity.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

namespace zenthrildb {

class WriteAheadLog {
public:
  WriteAheadLog() = default;
  explicit WriteAheadLog(std::filesystem::path path);
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
  void open(const std::filesystem::path& path, bool allowCreate, bool readOnly = false);
  [[nodiscard]] std::vector<LogRecord> readAll(
    const std::function<void(const LogRecord&)>& validate,
    const std::optional<WalIdentity>* expectedIdentity, bool repair);
  void repairPendingTail();
  void ensureOpen() const;
  void truncateIncompleteTail(std::uintmax_t validBytes);

  mutable std::recursive_mutex mutex_;
  std::filesystem::path path_{};
  std::fstream stream_{};
  bool createdOnOpen_{false};
  bool incompleteTailRepaired_{false};
  std::optional<std::uintmax_t> incompleteTailOffset_;
  bool hasIdentity_{false};
  bool readOnly_{false};
};

} // namespace zenthrildb
