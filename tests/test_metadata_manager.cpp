#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "core/MetadataManager.hpp"

#include <cassert>
#include <filesystem>

using namespace zenthrildb;

void test_metadata_manager() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_metadata.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);
  FreePageManager freePages(fm);
  freePages.load();

  MetadataManager metadata(fm, freePages);
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
  FreePageManager reopenedFreePages(reopened);
  MetadataManager recovered(reopened, reopenedFreePages);
  recovered.loadMetadata();
  assert(recovered.tableExists("ledger"));
  const auto recoveredTable = recovered.getTable("ledger");
  assert(recoveredTable.has_value());
  assert(recoveredTable->recordCount() == 12);
  assert(recovered.pageDirectory().get(recoveredTable->rootPageId()) != nullptr);

  assert(recovered.deleteTable("ledger"));
  assert(!recovered.tableExists("ledger"));

  reopened.closeDatabase();
  std::filesystem::remove(path);
}
