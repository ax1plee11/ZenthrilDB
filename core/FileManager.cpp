#include "core/FileManager.hpp"

#include "core/CRCManager.hpp"

#include <array>
#include <chrono>
#include <random>
#include <stdexcept>
#include <span>

namespace zenthrildb {

namespace {
DatabaseHeader createNewDatabaseHeader() {
  DatabaseHeader header{};
  header.creationTimestampUnixNs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());

  std::random_device rd;
  for (auto& byte : header.databaseUuid) {
    byte = static_cast<std::uint8_t>(rd());
  }
  return header;
}
} // namespace

FileManager::~FileManager() {
  closeDatabase();
}

void FileManager::createDatabase(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open()) {
    fileStream_.flush();
    fileStream_.close();
  }
  path_ = path;
  header_ = createNewDatabaseHeader();
  header_.checksum = 0;

  fileStream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!fileStream_) {
    throw std::runtime_error("Failed to create database file");
  }

  writeHeader();

  Page headerMirrorPage;
  headerMirrorPage.header().pageId = kHeaderMirrorPageId;
  headerMirrorPage.header().pageType = PageType::Metadata;
  writePage(headerMirrorPage);

  Page metadataPage;
  metadataPage.header().pageId = kDatabaseMetadataPageId;
  metadataPage.header().pageType = PageType::Metadata;
  writePage(metadataPage);

  Page directoryPage;
  directoryPage.header().pageId = kPageDirectoryPageId;
  directoryPage.header().pageType = PageType::Metadata;
  writePage(directoryPage);

  Page freeListPage;
  freeListPage.header().pageId = kFreePageListPageId;
  freeListPage.header().pageType = PageType::Free;
  writePage(freeListPage);

  fileStream_.flush();
}

void FileManager::openDatabase(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open()) {
    fileStream_.flush();
    fileStream_.close();
  }
  path_ = path;
  fileStream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!fileStream_) {
    throw std::runtime_error("Failed to open database file");
  }
  readHeader();
}

void FileManager::closeDatabase() {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open()) {
    fileStream_.flush();
    fileStream_.close();
  }
}

Page FileManager::readPage(PageId pageId) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  Page page;
  fileStream_.seekg(pageOffset(pageId));
  if (!fileStream_) {
    throw std::runtime_error("Failed to seek to page");
  }

  std::array<Byte, kDefaultPageSize> bytes{};
  fileStream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (fileStream_.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("Failed to read complete page");
  }

  page.deserialize(bytes);
  auto checksumPage = page;
  checksumPage.header().checksum = 0;
  const auto serialized = checksumPage.serialize();
  const auto actualChecksum = CRCManager::compute(std::span<const Byte>(serialized.data(), serialized.size()));
  if (actualChecksum != page.header().checksum) {
    throw std::runtime_error("Page checksum verification failed");
  }
  return page;
}

void FileManager::writePage(const Page& page) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  auto pageToWrite = page;
  pageToWrite.header().checksum = 0;
  const auto checksumBytes = pageToWrite.serialize();
  pageToWrite.header().checksum = CRCManager::compute(std::span<const Byte>(checksumBytes.data(), checksumBytes.size()));
  const auto bytes = pageToWrite.serialize();
  fileStream_.seekp(pageOffset(page.header().pageId));
  if (!fileStream_) {
    throw std::runtime_error("Failed to seek for page write");
  }
  fileStream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!fileStream_) {
    throw std::runtime_error("Failed to write page");
  }
  const auto pageCount = static_cast<std::uint64_t>(page.header().pageId) + 1;
  if (pageCount > header_.totalPages) {
    header_.totalPages = pageCount;
    writeHeader();
  }
}

PageId FileManager::reservePageId() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  const auto nextPageId = static_cast<PageId>(header_.totalPages);
  ++header_.totalPages;
  writeHeader();
  return nextPageId;
}

void FileManager::flush() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  fileStream_.flush();
}

void FileManager::ensureOpen() const {
  if (!fileStream_.is_open()) {
    throw std::runtime_error("Database file is not open");
  }
}

void FileManager::writeHeader() {
  header_.checksum = 0;
  const auto checksumBytes = header_.serialize();
  header_.checksum = CRCManager::compute(std::span<const Byte>(checksumBytes.data(), checksumBytes.size()));
  fileStream_.seekp(0);
  const auto bytes = header_.serialize();
  fileStream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!fileStream_) {
    throw std::runtime_error("Failed to write database header");
  }
}

void FileManager::readHeader() {
  fileStream_.seekg(0);
  std::array<Byte, DatabaseHeader::serializedSize()> bytes{};
  fileStream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!fileStream_) {
    throw std::runtime_error("Failed to read database header");
  }
  header_.deserialize(bytes);
  const auto checksum = header_.checksum;
  header_.checksum = 0;
  const auto checksumBytes = header_.serialize();
  const auto actualChecksum = CRCManager::compute(std::span<const Byte>(checksumBytes.data(), checksumBytes.size()));
  header_.checksum = checksum;
  if (actualChecksum != checksum) {
    throw std::runtime_error("Database header checksum verification failed");
  }
}

std::streamoff FileManager::pageOffset(PageId pageId) noexcept {
  return static_cast<std::streamoff>(DatabaseHeader::serializedSize()) +
         static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(kDefaultPageSize);
}

} // namespace zenthrildb
