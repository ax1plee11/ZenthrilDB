#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"
#include "core/StorageConsistencyChecker.hpp"
#include "tests/TestDatabaseFiles.hpp"
#include "transaction/StorageMutationCoordinator.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_metadata_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_metadata.zdb";
  removeTestDatabaseArtifacts(path);

  FileManager fm;
  fm.createDatabase(path);
  LogManager metadataWal(databaseWalPath(path));
  TransactionManager transactions(metadataWal);
  StorageMutationCoordinator mutationCoordinator(fm, metadataWal, transactions);
  FreePageManager freePages(fm, mutationCoordinator);
  freePages.load();

  MetadataManager metadata(fm, freePages, mutationCoordinator);
  const auto table = metadata.createTable("accounts");
  assert(metadata.tableExists("accounts"));
  assert(table.name() == "accounts");
  assert(table.rootPageId() >= kFirstUserPageId);
  assert(metadata.pageDirectory().get(table.rootPageId()) != nullptr);

  assert(metadata.renameTable("accounts", "ledger"));
  assert(!metadata.tableExists("accounts"));
  assert(metadata.tableExists("ledger"));
  const auto renamed = metadata.getTable("ledger");
  assert(renamed.has_value());
  assert(metadata.pageDirectory().get(renamed->rootPageId()) != nullptr);

  auto updated = *renamed;
  updated.setRecordCount(12);
  updated.setPageCount(2);
  metadata.updateMetadata(updated);
  const auto afterUpdate = metadata.getTable("ledger");
  assert(afterUpdate.has_value());
  assert(afterUpdate->recordCount() == 12);
  assert(afterUpdate->pageCount() == 2);

  metadata.saveMetadata();
  fm.closeDatabase();

  FileManager reopened;
  reopened.openDatabase(path);
  LogManager recoveredWal(databaseWalPath(path));
  TransactionManager recoveredTransactions(recoveredWal);
  StorageMutationCoordinator recoveredCoordinator(reopened, recoveredWal, recoveredTransactions);
  FreePageManager reopenedFreePages(reopened, recoveredCoordinator);
  MetadataManager recovered(reopened, reopenedFreePages, recoveredCoordinator);
  recovered.loadMetadata();
  assert(recovered.tableExists("ledger"));
  const auto recoveredTable = recovered.getTable("ledger");
  assert(recoveredTable.has_value());
  assert(recoveredTable->recordCount() == 12);
  assert(recovered.pageDirectory().get(recoveredTable->rootPageId()) != nullptr);

  const auto consistency = StorageConsistencyChecker{}.check(
    reopened, reopenedFreePages, recovered, recoveredWal);
  assert(consistency.pagesChecked == reopened.header().totalPages);

  assert(recovered.deleteTable("ledger"));
  assert(!recovered.tableExists("ledger"));
  const auto afterDeleteConsistency = StorageConsistencyChecker{}.check(
    reopened, reopenedFreePages, recovered, recoveredWal);
  assert(afterDeleteConsistency.transactionsChecked >= 4);

  reopened.closeDatabase();
  FileManager afterDelete;
  afterDelete.openDatabase(path);
  LogManager afterDeleteWal(databaseWalPath(path));
  TransactionManager afterDeleteTransactions(afterDeleteWal);
  StorageMutationCoordinator afterDeleteCoordinator(afterDelete, afterDeleteWal, afterDeleteTransactions);
  FreePageManager afterDeleteFreePages(afterDelete, afterDeleteCoordinator);
  afterDeleteFreePages.load();
  MetadataManager afterDeleteMetadata(afterDelete, afterDeleteFreePages, afterDeleteCoordinator);
  afterDeleteMetadata.loadMetadata();
  assert(!afterDeleteMetadata.tableExists("ledger"));
  assert(afterDeleteFreePages.freePages().contains(recoveredTable->rootPageId()));
  (void)StorageConsistencyChecker{}.check(afterDelete, afterDeleteFreePages, afterDeleteMetadata, afterDeleteWal);
  afterDelete.closeDatabase();
  removeTestDatabaseArtifacts(path);
}
