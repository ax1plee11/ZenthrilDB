#include "core/FileManager.hpp"
#include "core/FreePageManager.hpp"
#include "storage/BinaryFormat.hpp"
#include "storage/DatabaseMetadata.hpp"
#include "storage/PageDirectory.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <stdexcept>

using namespace zenthrildb;

namespace {

template <typename Exception, typename Fn>
void expectThrow(Fn&& fn) {
  bool threw = false;
  try {
    fn();
  } catch (const Exception&) {
    threw = true;
  }
  assert(threw);
}

void writeU32(Byte* data, std::size_t offset, std::uint32_t value) {
  std::memcpy(data + offset, &value, sizeof(value));
}

} // namespace

void test_storage_boundaries() {
  DatabaseMetadata duplicateTables;
  duplicateTables.upsertTable(TableMetadata{1, "users", kFirstUserPageId});
  const auto serializedTables = duplicateTables.serialize();
  auto duplicateTableBytes = serializedTables;
  const auto firstTableOffset = format::kDatabaseMetadataEntriesOffset;
  std::memcpy(duplicateTableBytes.data() + firstTableOffset + TableMetadata::kSerializedSize,
              duplicateTableBytes.data() + firstTableOffset,
              TableMetadata::kSerializedSize);
  writeU32(duplicateTableBytes.data(), format::kDatabaseMetadataTableCountOffset, 2);
  expectThrow<std::invalid_argument>([&] {
    DatabaseMetadata restored;
    restored.deserialize(std::span<const Byte>(duplicateTableBytes.data(), duplicateTableBytes.size()));
  });

  auto badMetadataVersion = serializedTables;
  writeU32(badMetadataVersion.data(), format::kDatabaseMetadataVersionOffset, 99);
  expectThrow<std::invalid_argument>([&] {
    DatabaseMetadata restored;
    restored.deserialize(std::span<const Byte>(badMetadataVersion.data(), badMetadataVersion.size()));
  });

  auto overflowTableCount = serializedTables;
  writeU32(overflowTableCount.data(), format::kDatabaseMetadataTableCountOffset, 100000);
  expectThrow<std::invalid_argument>([&] {
    DatabaseMetadata restored;
    restored.deserialize(std::span<const Byte>(overflowTableCount.data(), overflowTableCount.size()));
  });

  PageDirectory directory;
  directory.registerPage(PageDirectoryEntry{
    .pageId = kFirstUserPageId,
    .pageType = PageType::Table,
    .ownerTable = "users",
    .creationTimestampUnixNs = 1,
    .status = PageStatus::InUse,
  });
  const auto directoryBytes = directory.serialize();
  auto duplicateDirectoryBytes = directoryBytes;
  std::memcpy(duplicateDirectoryBytes.data() + format::kPageDirectoryEntriesOffset + PageDirectoryEntry::serializedSize(),
              duplicateDirectoryBytes.data() + format::kPageDirectoryEntriesOffset,
              PageDirectoryEntry::serializedSize());
  writeU32(duplicateDirectoryBytes.data(), format::kPageDirectoryCountOffset, 2);
  expectThrow<std::invalid_argument>([&] {
    PageDirectory restored;
    restored.deserialize(std::span<const Byte>(duplicateDirectoryBytes.data(), duplicateDirectoryBytes.size()));
  });

  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_boundaries.zdb";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }

  FileManager fm;
  fm.createDatabase(path);
  FreePageManager freePages(fm);
  expectThrow<std::invalid_argument>([&] {
    freePages.freePage(kDatabaseMetadataPageId);
  });
  freePages.freePage(kFirstUserPageId);
  freePages.freePage(kFirstUserPageId);
  assert(freePages.freePages().size() == 1);

  fm.closeDatabase();
  std::filesystem::remove(path);
}
