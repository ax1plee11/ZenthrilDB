#include "core/CRCManager.hpp"
#include "core/DatabaseRuntime.hpp"
#include "tests/TestDatabaseFiles.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

using namespace zenthrildb;

namespace {
void createLegacy(const std::filesystem::path& path, bool insert) {
  FileManager files;
  files.createDatabase(path);
  LogManager wal(databaseWalPath(path));
  TransactionManager transactions(wal);
  StorageMutationCoordinator mutation(files, wal, transactions);
  FreePageManager freePages(files, mutation);
  freePages.load();
  MetadataManager metadata(files, freePages, mutation);
  metadata.saveMetadata();
  if (insert) {
    PageManager pages(files, freePages, metadata);
    std::recursive_mutex mutex;
    RecordManager records(pages, metadata, mutex);
    (void)records.createTable("events");
    records.put("events", "confirmed", std::vector<Byte>{1, 2, 3});
  }
}

std::string readBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Failed to read test fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void assertOpenRejected(const std::filesystem::path& path, std::string_view expected) {
  bool rejected = false;
  try { DatabaseRuntime runtime(path); }
  catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).find(expected) != std::string_view::npos; }
  assert(rejected);
}
} // namespace

void test_database_wal_identity() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("zenthrildb_wal_identity_" + std::to_string(stamp));
  assert(std::filesystem::create_directory(directory));
  const auto source = directory / "source.zdb";
  const auto other = directory / "other.zdb";
  const auto restored = directory / "restored.zdb";
  const auto incompatible = directory / "incompatible.zdb";
  const auto sourceWal = databaseWalPath(source);
  const auto otherWal = databaseWalPath(other);
  const auto restoredWal = databaseWalPath(restored);
  const auto incompatibleWal = databaseWalPath(incompatible);
  const std::vector<Byte> value{1, 2, 3};
  createLegacy(source, true);
  createLegacy(other, false);
  std::filesystem::copy_file(sourceWal, otherWal, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(source, restored);
  std::filesystem::copy_file(sourceWal, restoredWal);
  const auto completeWal = readBytes(sourceWal);
  for (const auto& wal : {otherWal, restoredWal}) {
    std::ofstream tail(wal, std::ios::binary | std::ios::app);
    tail.write(completeWal.data(), 3); // A partial next frame header.
    tail.close();
    assert(tail);
  }

  const auto originalData = readBytes(other);
  const auto foreignWal = readBytes(otherWal);
  assertOpenRejected(other, "WAL database UUID mismatch");
  assert(readBytes(other) == originalData);
  assert(readBytes(otherWal) == foreignWal);

  {
    DatabaseRuntime runtime(restored);
    assert(runtime.records().get("events", "confirmed") == value);
  }
  assert(readBytes(restoredWal) == completeWal);

  std::filesystem::copy_file(source, incompatible);
  std::filesystem::copy_file(sourceWal, incompatibleWal);
  {
    const auto contents = readBytes(incompatible);
    DatabaseHeader header;
    header.deserialize({reinterpret_cast<const Byte*>(contents.data()), DatabaseHeader::kSerializedSize});
    ++header.version;
    header.checksum = 0;
    header.checksum = CRCManager::compute(header.serialize());
    const auto image = header.serialize();
    std::fstream file(incompatible, std::ios::binary | std::ios::in | std::ios::out);
    file.write(reinterpret_cast<const char*>(image.data()), image.size());
    file.close();
    assert(file);
  }
  assertOpenRejected(incompatible, "Unsupported or invalid database header");
  for (const auto& path : {source, other, restored, incompatible}) removeTestDatabaseArtifacts(path);
  std::filesystem::remove(directory);
}
