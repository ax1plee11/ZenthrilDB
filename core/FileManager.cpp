#include "core/FileManager.hpp"

#include "core/CRCManager.hpp"
#include "recovery/RecoveryCoordinator.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "storage/IoFaultInjector.hpp"
#include "storage/SnapshotManifest.hpp"
#include "wal/LogManager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>
#include <span>
#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zenthrildb {

namespace {
void createDatabaseFileExclusive(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Database already exists or cannot be created");
  if (!CloseHandle(handle)) throw std::runtime_error("Failed to close newly created database file");
#else
  const auto descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (descriptor < 0) throw std::runtime_error("Database already exists or cannot be created");
  if (::close(descriptor) != 0) throw std::runtime_error("Failed to close newly created database file");
#endif
}

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
  try { closeDatabase(); } catch (...) { }
}

void FileManager::createDatabase(const std::filesystem::path& path) {
  createDatabase(path, false);
}

void FileManager::createDatabase(const std::filesystem::path& path, bool bindWal) {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open() || processLock_) closeDatabase();
  auto processLock = std::make_unique<DatabaseFileLock>(path);
  if (std::filesystem::exists(path)) throw std::runtime_error("Refusing to overwrite an existing database file");
  if (std::filesystem::exists(databaseWalPath(path))) {
    throw std::runtime_error("Refusing to reuse an existing WAL for a new database");
  }
  if (std::filesystem::exists(snapshotBaselinePath(path)) || std::filesystem::exists(snapshotManifestPath(path))) {
    throw std::runtime_error("Refusing to reuse existing snapshot files for a new database");
  }
  path_ = path;
  header_ = createNewDatabaseHeader();
  walIdentity_.reset();
  header_.checksum = 0;

  createDatabaseFileExclusive(path_);
  fileStream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!fileStream_) {
    throw std::runtime_error("Failed to create database file");
  }

  writeHeader();

  Page headerMirrorPage;
  headerMirrorPage.header().pageId = kHeaderMirrorPageId;
  headerMirrorPage.header().pageType = PageType::Metadata;
  writePage(bindWal ? WalIdentity::fresh(header_, 0).toPage() : headerMirrorPage);
  if (bindWal) readIdentity();

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

  flush();
  detail::syncDirectoryToStorage(path_.parent_path());
  {
    const auto walPath = databaseWalPath(path_);
    createDatabaseFileExclusive(walPath);
    if (walIdentity_) {
      std::ofstream wal(walPath, std::ios::binary | std::ios::app);
      const auto bytes = walIdentity_->serialize();
      wal.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      wal.close();
      if (!wal) throw std::runtime_error("Failed to initialize WAL identity header");
    }
    detail::syncFileToStorage(walPath);
  }
  detail::syncDirectoryToStorage(path_.parent_path());
  processLock_ = std::move(processLock);
  failed_ = false;
}

void FileManager::openDatabase(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open() || processLock_) closeDatabase();
  auto processLock = std::make_unique<DatabaseFileLock>(path);
  path_ = path;
  walIdentity_.reset();
  if (!std::filesystem::is_regular_file(path_)) throw std::runtime_error("Database is missing or is not a regular file");
  fileStream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!fileStream_) {
    throw std::runtime_error("Failed to open database file");
  }
  try {
    readHeader();
    readIdentity();
    const auto walPath = databaseWalPath(path_);
    if (!std::filesystem::exists(walPath)) {
      throw std::runtime_error("Database WAL is missing; refusing to expose database");
    }
    LogManager wal(walPath, header_, walIdentity_);
    RecoveryCoordinator coordinator;
    (void)coordinator.recover(wal, *this);
    verifyPhysicalLayout();
    processLock_ = std::move(processLock);
    failed_ = false;
  } catch (...) {
    fileStream_.close();
    header_ = {};
    walIdentity_.reset();
    recoverySnapshotBoundary_.reset();
    throw;
  }
}

void FileManager::openReadOnly(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (fileStream_.is_open()) throw std::logic_error("An open file manager cannot become read-only");
  if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Snapshot data file is missing");
  path_ = path;
  fileStream_.open(path_, std::ios::binary | std::ios::in);
  if (!fileStream_) throw std::runtime_error("Cannot read snapshot data file");
  readOnly_ = true;
  readHeader();
  readIdentity();
}

void FileManager::closeDatabase() {
  std::lock_guard lock(mutex_);
  std::exception_ptr failure;
  if (fileStream_.is_open()) {
    if (!readOnly_) {
      try { flush(); } catch (...) { failure = std::current_exception(); }
    }
    fileStream_.close();
    if (!failure && fileStream_.fail()) failure = std::make_exception_ptr(std::runtime_error("Failed to close database file"));
  }
  processLock_.reset();
  walIdentity_.reset();
  recoverySnapshotBoundary_.reset();
  failed_ = false;
  readOnly_ = false;
  if (failure) std::rethrow_exception(failure);
}

Page FileManager::readPage(PageId pageId) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  const auto fileSize = std::filesystem::file_size(path_);
  const auto offset = static_cast<std::uintmax_t>(pageOffset(pageId));
  if (offset > fileSize || kDefaultPageSize > fileSize - offset) {
    throw PageNotFoundError{};
  }
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
  return page;
}

void FileManager::writePage(const Page& page) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot write a read-only database");
  if (walIdentity_ && page.header().pageId == kHeaderMirrorPageId) {
    throw std::invalid_argument("The database identity page is immutable");
  }
  const auto bytes = page.serialize();
  IoFaultInjector::failIfArmed(IoFaultPoint::NoSpace);
  IoFaultInjector::failIfArmed(IoFaultPoint::PageWrite);
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
    const auto previousHeader = header_;
    header_.totalPages = pageCount;
    try { writeHeader(); }
    catch (...) { header_ = previousHeader; throw; }
  }
}

PageId FileManager::reservePageId() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot reserve a page in a read-only database");
  if (header_.totalPages > static_cast<std::uint64_t>(std::numeric_limits<PageId>::max())) {
    throw std::overflow_error("Page identifier space exhausted");
  }
  const auto previousHeader = header_;
  const auto nextPageId = static_cast<PageId>(header_.totalPages);
  ++header_.totalPages;
  try { writeHeader(); }
  catch (...) { header_ = previousHeader; throw; }
  return nextPageId;
}

std::uint64_t FileManager::physicalPageCount() const {
  const auto layout = physicalLayout();
  if (layout.hasPartialPage) throw std::runtime_error("Database physical file has a partial page");
  return layout.completePages;
}

FileManager::PhysicalLayout FileManager::physicalLayout() const {
  std::lock_guard lock(mutex_);
  ensureOpen();
  const auto size = std::filesystem::file_size(path_);
  const auto prefix = static_cast<std::uintmax_t>(DatabaseHeader::serializedSize());
  if (size < prefix) throw std::runtime_error("Database physical file is missing its header");
  const auto count = (size - prefix) / kDefaultPageSize;
  const auto partial = (size - prefix) % kDefaultPageSize != 0;
  if (count + partial > static_cast<std::uint64_t>(std::numeric_limits<PageId>::max()) + 1) {
    throw std::runtime_error("Database physical page count exceeds addressable page ids");
  }
  return {count, partial};
}

void FileManager::verifyPhysicalLayout() const {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (physicalPageCount() != header_.totalPages) {
    throw std::runtime_error("Database header page count does not match physical file size after recovery");
  }
}

void FileManager::restoreHeaderImage(
    const std::array<Byte, DatabaseHeader::serializedSize()>& image, bool truncateTail) {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot restore a read-only database header");
  DatabaseHeader restored;
  restored.deserialize(image);
  const auto expectedChecksum = restored.checksum;
  restored.checksum = 0;
  const auto checksumBytes = restored.serialize();
  if (restored.magic != kDatabaseMagic || restored.version != kCurrentDatabaseVersion ||
      restored.pageSize != kDefaultPageSize ||
      restored.totalPages < kFirstUserPageId ||
      restored.totalPages > static_cast<std::uint64_t>(std::numeric_limits<PageId>::max()) + 1 ||
      CRCManager::compute(checksumBytes) != expectedChecksum) {
    throw std::runtime_error("Recovery refused an invalid database header image");
  }
  if (restored.databaseUuid != header_.databaseUuid) {
    throw std::runtime_error("Recovery refused a database header image with a different UUID");
  }
  restored.checksum = expectedChecksum;
  const auto previousHeader = header_;
  header_ = restored;
  try {
    writeHeader();
    if (truncateTail) {
      fileStream_.flush();
      if (!fileStream_) throw std::runtime_error("Failed to flush before database truncation");
      const auto targetSize = static_cast<std::uintmax_t>(DatabaseHeader::serializedSize()) +
        header_.totalPages * static_cast<std::uintmax_t>(kDefaultPageSize);
      fileStream_.close();
      std::filesystem::resize_file(path_, targetSize);
      fileStream_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
      if (!fileStream_) throw std::runtime_error("Failed to reopen database after recovery truncation");
    }
  } catch (...) {
    header_ = previousHeader;
    throw;
  }
}

void FileManager::flush() {
  std::lock_guard lock(mutex_);
  ensureOpen();
  if (readOnly_) throw std::logic_error("Cannot synchronize a read-only database");
  fileStream_.flush();
  if (!fileStream_) {
    throw std::runtime_error("Failed to flush database stream");
  }
  IoFaultInjector::failIfArmed(IoFaultPoint::PageSync);
  detail::syncFileToStorage(path_);
}

void FileManager::ensureOpen() const {
  if (!fileStream_.is_open()) {
    throw std::runtime_error("Database file is not open");
  }
  if (failed_) throw std::runtime_error("Database handle is unusable after a failed storage mutation; close and reopen it");
}

void FileManager::writeHeader() {
  header_.checksum = 0;
  const auto checksumBytes = header_.serialize();
  header_.checksum = CRCManager::compute(std::span<const Byte>(checksumBytes.data(), checksumBytes.size()));
  IoFaultInjector::failIfArmed(IoFaultPoint::NoSpace);
  IoFaultInjector::failIfArmed(IoFaultPoint::HeaderWrite);
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
  if (header_.magic != kDatabaseMagic || header_.version != kCurrentDatabaseVersion ||
      header_.pageSize != kDefaultPageSize || header_.totalPages < kFirstUserPageId ||
      header_.totalPages > static_cast<std::uint64_t>(std::numeric_limits<PageId>::max()) + 1) {
    throw std::runtime_error("Unsupported or invalid database header");
  }
}

void FileManager::readIdentity() {
  const auto page = readPage(kHeaderMirrorPageId);
  if (page.header().pageId != kHeaderMirrorPageId || page.header().pageType != PageType::Metadata) {
    throw std::runtime_error("Invalid database identity page");
  }
  if (page.header().version == Page::kFormatVersionV1 && page.header().usedBytes == 0 &&
      std::ranges::all_of(page.payload(), [](Byte value) { return value == 0; })) return;
  if (page.header().version != Page::kFormatVersionV2 || page.header().usedBytes != WalIdentity::kSize ||
      page.pageLsn() != 0) throw std::runtime_error("Invalid database identity page");
  const auto identity = WalIdentity::deserialize(page.payload().first(WalIdentity::kSize));
  if (identity.databaseUuid != header_.databaseUuid) throw std::runtime_error("Database identity page UUID mismatch");
  walIdentity_ = identity;
}

std::streamoff FileManager::pageOffset(PageId pageId) noexcept {
  return static_cast<std::streamoff>(DatabaseHeader::serializedSize()) +
         static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(kDefaultPageSize);
}

} // namespace zenthrildb
