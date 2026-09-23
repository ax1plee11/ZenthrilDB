#pragma once

#include "storage/DatabaseHeader.hpp"
#include "storage/Page.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace zenthrildb {

class FileManager {
public:
  FileManager() = default;
  ~FileManager();

  void createDatabase(const std::filesystem::path& path);
  void openDatabase(const std::filesystem::path& path);
  void closeDatabase();

  [[nodiscard]] Page readPage(PageId pageId);
  void writePage(const Page& page);
  void flush();
  [[nodiscard]] PageId reservePageId();

  [[nodiscard]] const DatabaseHeader& header() const noexcept { return header_; }
  [[nodiscard]] bool isOpen() const noexcept { return fileStream_.is_open(); }

private:
  void ensureOpen() const;
  void writeHeader();
  void readHeader();
  static std::streamoff pageOffset(PageId pageId) noexcept;

  mutable std::recursive_mutex mutex_;
  std::fstream fileStream_;
  std::filesystem::path path_;
  DatabaseHeader header_{};
};

} // namespace zenthrildb
