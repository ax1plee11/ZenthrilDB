#pragma once

#include "wal/LogRecord.hpp"

#include <filesystem>
#include <fstream>
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
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  void ensureOpen() const;

  mutable std::mutex mutex_;
  std::filesystem::path path_{};
  std::fstream stream_{};
};

} // namespace zenthrildb
