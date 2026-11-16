#include "core/FileManager.hpp"
#include "storage/IoFaultInjector.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace zenthrildb;

void test_storage_faults() {
#ifdef ZENTHRILDB_ENABLE_FAULT_INJECTION
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto databasePath = std::filesystem::temp_directory_path() /
    ("zenthrildb_fault_" + std::to_string(stamp) + ".zdb");
  removeTestDatabaseArtifacts(databasePath);
  {
    FileManager files;
    files.createDatabase(databasePath);
    const auto originalPageCount = files.header().totalPages;

    IoFaultInjector::arm(IoFaultPoint::HeaderWrite);
    bool headerFailed = false;
    try { (void)files.reservePageId(); }
    catch (const std::system_error&) { headerFailed = true; }
    IoFaultInjector::clear();
    assert(headerFailed);
    assert(files.header().totalPages == originalPageCount);

    Page page;
    page.header().pageId = kFirstUserPageId;
    page.header().pageType = PageType::Table;
    IoFaultInjector::arm(IoFaultPoint::PageWrite);
    bool pageWriteFailed = false;
    try { files.writePage(page); }
    catch (const std::system_error&) { pageWriteFailed = true; }
    IoFaultInjector::clear();
    assert(pageWriteFailed);

    files.writePage(page);
    IoFaultInjector::arm(IoFaultPoint::PageSync);
    bool pageSyncFailed = false;
    try { files.flush(); }
    catch (const std::system_error&) { pageSyncFailed = true; }
    IoFaultInjector::clear();
    assert(pageSyncFailed);

    IoFaultInjector::arm(IoFaultPoint::NoSpace);
    bool noSpacePropagated = false;
    try { files.writePage(page); }
    catch (const std::system_error& error) {
      noSpacePropagated = error.code() == std::error_code(ENOSPC, std::generic_category());
    }
    IoFaultInjector::clear();
    assert(noSpacePropagated);
    files.closeDatabase();
  }

  const auto walPath = std::filesystem::temp_directory_path() /
    ("zenthrildb_fault_" + std::to_string(stamp) + ".wal");
  {
    LogManager log(walPath);
    IoFaultInjector::arm(IoFaultPoint::WalPartialWrite);
    bool partialWriteFailed = false;
    try { (void)log.appendWithLsn(LogRecordType::DatabaseOpened, [](LogSequenceNumber) {
      return std::vector<Byte>{0xAA};
    }); }
    catch (const std::system_error&) { partialWriteFailed = true; }
    IoFaultInjector::clear();
    assert(partialWriteFailed);
  }
  {
    LogManager repaired(walPath);
    assert(repaired.replay().empty());
  }
  {
    LogManager log(walPath);
    IoFaultInjector::arm(IoFaultPoint::WalSync);
    bool syncFailed = false;
    try { (void)log.appendWithLsn(LogRecordType::DatabaseOpened, [](LogSequenceNumber) {
      return std::vector<Byte>{0xBB};
    }); }
    catch (const std::system_error&) { syncFailed = true; }
    IoFaultInjector::clear();
    assert(syncFailed);
  }
  std::filesystem::remove(walPath);
  removeTestDatabaseArtifacts(databasePath);
#else
  // Fault injection is intentionally absent from Release builds.
#endif
}
