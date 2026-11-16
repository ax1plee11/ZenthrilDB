#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "storage/DatabaseWalPath.hpp"
#include "transaction/StorageMutationCoordinator.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_free_page_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_freepages.zdb";
  removeTestDatabaseArtifacts(path);

  FileManager fm;
  fm.createDatabase(path);
  LogManager wal(databaseWalPath(path));
  TransactionManager transactions(wal);
  StorageMutationCoordinator coordinator(fm, wal, transactions);
  FreePageManager freePages(fm, coordinator);
  freePages.load();
  const auto freshPage = freePages.allocatePage();
  assert(freshPage >= kFirstUserPageId);
  freePages.freePage(freshPage);
  freePages.save();

  FreePageManager reopened(fm, coordinator);
  reopened.load();
  const auto reused = reopened.allocatePage();
  assert(reused == freshPage);

  fm.closeDatabase();
  removeTestDatabaseArtifacts(path);
}
