#include "core/RecordManager.hpp"

#include "core/MetadataManager.hpp"
#include "storage/BinaryIO.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace zenthrildb {
namespace {

constexpr std::uint32_t kRootMagic = 0x31544252; // RBT1
constexpr std::uint32_t kRecordMagic = 0x31434552; // REC1
constexpr std::uint32_t kRecordFormatVersion = 1;
constexpr std::uint32_t kTombstone = 1;
constexpr std::size_t kRootHeaderSize = 8;
constexpr std::size_t kRecordHeaderSize = 24;

template<typename T>
T readAt(std::span<const Byte> bytes, std::size_t offset) {
  return binary::readLittleEndian<T>(bytes, offset);
}

template<typename T>
void writeAt(std::span<Byte> bytes, std::size_t offset, T value) {
  binary::writeLittleEndian<T>(bytes, offset, value);
}

} // namespace

RecordManager::RecordManager(PageManager& pages, MetadataManager& metadata, std::recursive_mutex& runtimeMutex)
  : pages_(pages), metadata_(metadata), mutex_(runtimeMutex) {}

TableMetadata RecordManager::createTable(std::string_view table) {
  if (table.empty()) throw std::invalid_argument("Table name cannot be empty");
  std::lock_guard lock(mutex_);
  return metadata_.createTable(std::string(table));
}

bool RecordManager::renameTable(std::string_view currentName, std::string_view newName) {
  if (newName.empty()) throw std::invalid_argument("Table name cannot be empty");
  std::lock_guard lock(mutex_);
  return metadata_.renameTable(std::string(currentName), std::string(newName));
}

bool RecordManager::dropTable(std::string_view table) {
  std::lock_guard lock(mutex_);
  return metadata_.deleteTable(std::string(table));
}

void RecordManager::put(std::string_view tableName, std::string_view key, std::span<const Byte> value) {
  if (key.empty()) throw std::invalid_argument("Record key cannot be empty");
  if (key.size() > std::numeric_limits<std::uint32_t>::max() || value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("Record key or value is too large");
  }
  if (kRecordHeaderSize + key.size() + value.size() > Page::kV2PayloadSize) {
    throw std::length_error("Record does not fit in one page");
  }
  std::lock_guard lock(mutex_);
  const auto table = requireTable(tableName);
  auto root = pages_.readPage(table.rootPageId());
  initializeRoot(root);

  if (auto existing = find(table, key)) {
    auto payload = existing->page.payload();
    if (!value.empty()) std::memcpy(payload.data() + existing->valueOffset, value.data(), value.size());
    writeAt<std::uint32_t>(payload, 16, static_cast<std::uint32_t>(value.size()));
    writeAt<std::uint32_t>(payload, 20, 0);
    existing->page.header().usedBytes = static_cast<std::uint32_t>(kRecordHeaderSize + key.size() + value.size());
    pages_.writePage(existing->page);
    return;
  }

  const auto head = root.header().usedBytes == 0 ? PageId{0} : readAt<PageId>(root.payload(), 4);
  Page recordPage;
  recordPage.header().version = Page::kFormatVersionV2;
  recordPage.header().pageType = PageType::Overflow;
  auto payload = recordPage.payload();
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(payload, offset, kRecordMagic);
  binary::writeLittleEndian<PageId>(payload, offset, head);
  binary::writeLittleEndian<std::uint32_t>(payload, offset, kRecordFormatVersion);
  binary::writeLittleEndian<std::uint32_t>(payload, offset, static_cast<std::uint32_t>(key.size()));
  binary::writeLittleEndian<std::uint32_t>(payload, offset, static_cast<std::uint32_t>(value.size()));
  binary::writeLittleEndian<std::uint32_t>(payload, offset, 0);
  std::memcpy(payload.data() + offset, key.data(), key.size());
  offset += key.size();
  if (!value.empty()) std::memcpy(payload.data() + offset, value.data(), value.size());
  offset += value.size();
  recordPage.header().usedBytes = static_cast<std::uint32_t>(offset);
  pages_.allocatePageAndWrite(std::move(recordPage), std::string(tableName), std::move(root),
    [](Page& rootPage, PageId newHead) {
      writeAt<std::uint32_t>(rootPage.payload(), 0, kRootMagic);
      writeAt<PageId>(rootPage.payload(), 4, newHead);
      rootPage.header().usedBytes = static_cast<std::uint32_t>(kRootHeaderSize);
    });
}

std::optional<std::vector<Byte>> RecordManager::get(std::string_view tableName, std::string_view key) {
  std::lock_guard lock(mutex_);
  const auto table = requireTable(tableName);
  auto located = find(table, key);
  if (!located) return std::nullopt;
  const auto payload = located->page.payload();
  if (readAt<std::uint32_t>(payload, 20) == kTombstone) return std::nullopt;
  const auto value = payload.subspan(located->valueOffset, located->valueSize);
  return std::vector<Byte>(value.begin(), value.end());
}

bool RecordManager::erase(std::string_view tableName, std::string_view key) {
  std::lock_guard lock(mutex_);
  const auto table = requireTable(tableName);
  auto chain = readChain(table);
  for (std::size_t i = 0; i < chain.size(); ++i) {
    auto payload = chain[i].payload();
    const auto keySize = readAt<std::uint32_t>(payload, 12);
    if (keySize != key.size() ||
        std::memcmp(payload.data() + kRecordHeaderSize, key.data(), key.size()) != 0) continue;
    if (readAt<std::uint32_t>(payload, 20) == kTombstone) return false;

    const auto following = readAt<PageId>(payload, 4);
    Page predecessor;
    if (i == 0) {
      predecessor = pages_.readPage(table.rootPageId());
      writeAt<PageId>(predecessor.payload(), 4, following);
    } else {
      predecessor = chain[i - 1];
      writeAt<PageId>(predecessor.payload(), 4, following);
    }
    pages_.freePageAndWrite(chain[i].header().pageId, predecessor);
    return true;
  }
  return false;
}

std::vector<std::pair<std::string, std::vector<Byte>>> RecordManager::scan(std::string_view tableName) {
  std::lock_guard lock(mutex_);
  const auto table = requireTable(tableName);
  std::vector<std::pair<std::string, std::vector<Byte>>> result;
  for (const auto& page : readChain(table)) {
    auto payload = page.payload();
    if (readAt<std::uint32_t>(payload, 20) == kTombstone) continue;
    const auto keySize = readAt<std::uint32_t>(payload, 12);
    const auto valueSize = readAt<std::uint32_t>(payload, 16);
    const auto keyStart = kRecordHeaderSize;
    const auto valueStart = keyStart + keySize;
    const auto key = std::string(reinterpret_cast<const char*>(payload.data() + keyStart), keySize);
    const auto valueSpan = payload.subspan(valueStart, valueSize);
    result.emplace_back(key, std::vector<Byte>(valueSpan.begin(), valueSpan.end()));
  }
  return result;
}

void RecordManager::validate() {
  std::lock_guard lock(mutex_);
  for (const auto& name : metadata_.listTables()) {
    (void)readChain(requireTable(name));
  }
}

std::optional<RecordManager::LocatedRecord> RecordManager::find(const TableMetadata& table, std::string_view key) {
  for (auto page : readChain(table)) {
    const auto payload = page.payload();
    const auto keySize = readAt<std::uint32_t>(payload, 12);
    if (keySize != key.size()) continue;
    const auto* stored = reinterpret_cast<const char*>(payload.data() + kRecordHeaderSize);
    if (std::memcmp(stored, key.data(), key.size()) != 0) continue;
    const auto valueSize = readAt<std::uint32_t>(payload, 16);
    return LocatedRecord{std::move(page), static_cast<std::uint32_t>(kRecordHeaderSize + keySize), valueSize};
  }
  return std::nullopt;
}

std::vector<Page> RecordManager::readChain(const TableMetadata& table) {
  auto root = pages_.readPage(table.rootPageId());
  if (root.header().pageType != PageType::Table) throw std::runtime_error("Table root page has an invalid type");
  if (root.header().usedBytes == 0) return {};
  if (root.header().usedBytes < kRootHeaderSize ||
      readAt<std::uint32_t>(root.payload(), 0) != kRootMagic) {
    throw std::runtime_error("Unsupported or corrupt record table root format");
  }
  std::vector<Page> pages;
  std::unordered_set<PageId> visited;
  std::unordered_set<std::string> liveKeys;
  auto next = readAt<PageId>(root.payload(), 4);
  while (next != 0) {
    if (!visited.insert(next).second || pages.size() >= pages_.databasePageCount()) {
      throw std::runtime_error("Record page chain contains a cycle or exceeds database page count");
    }
    auto page = pages_.readPage(next);
    const auto info = pages_.pageInfo(next);
    if (page.header().pageType != PageType::Overflow || !info || info->ownerTable != table.name() ||
        page.header().usedBytes < kRecordHeaderSize || page.header().usedBytes > Page::kV2PayloadSize) {
      throw std::runtime_error("Record page has invalid type, owner, or payload length");
    }
    const auto payload = page.payload();
    std::size_t offset = 0;
    if (binary::readLittleEndian<std::uint32_t>(payload, offset) != kRecordMagic) {
      throw std::runtime_error("Record page magic mismatch");
    }
    const auto following = binary::readLittleEndian<PageId>(payload, offset);
    if (binary::readLittleEndian<std::uint32_t>(payload, offset) != kRecordFormatVersion) {
      throw std::runtime_error("Unsupported record format version");
    }
    const auto keySize = binary::readLittleEndian<std::uint32_t>(payload, offset);
    const auto valueSize = binary::readLittleEndian<std::uint32_t>(payload, offset);
    const auto flags = binary::readLittleEndian<std::uint32_t>(payload, offset);
    if (flags != 0 && flags != kTombstone) throw std::runtime_error("Unsupported record flags");
    if (static_cast<std::uint64_t>(offset) + keySize + valueSize != page.header().usedBytes || keySize == 0) {
      throw std::runtime_error("Record key/value lengths do not match page payload");
    }
    if (flags == 0) {
      const auto* keyData = reinterpret_cast<const char*>(payload.data() + offset);
      const std::string liveKey(keyData, keySize);
      if (!liveKeys.insert(liveKey).second) {
        throw std::runtime_error("Record table contains duplicate live keys");
      }
    }
    pages.push_back(std::move(page));
    next = following;
  }
  return pages;
}

TableMetadata RecordManager::requireTable(std::string_view tableName) const {
  const auto table = metadata_.getTable(std::string(tableName));
  if (!table) throw std::invalid_argument("Table does not exist");
  return *table;
}

void RecordManager::initializeRoot(Page& root) const {
  if (root.header().usedBytes == 0) return;
  if (root.header().usedBytes < kRootHeaderSize ||
      readAt<std::uint32_t>(root.payload(), 0) != kRootMagic) {
    throw std::runtime_error("Unsupported or corrupt record table root format");
  }
}

} // namespace zenthrildb
