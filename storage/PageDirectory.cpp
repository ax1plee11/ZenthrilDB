#include "storage/PageDirectory.hpp"

#include "storage/BinaryIO.hpp"
#include "storage/BinaryFormat.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace zenthrildb {

std::array<Byte, PageDirectoryEntry::kSerializedSize> PageDirectoryEntry::serialize() const {
  std::array<Byte, kSerializedSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<PageId>(bytes, offset, pageId);
  const auto pageTypeValue = static_cast<std::uint32_t>(pageType);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, pageTypeValue);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, creationTimestampUnixNs);
  const auto statusValue = static_cast<std::uint32_t>(status);
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, statusValue);
  const auto ownerLength = static_cast<std::uint32_t>(ownerTable.size());
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, ownerLength);
  if (ownerTable.size() > kMaxOwnerLength) {
    throw std::invalid_argument("Page directory owner exceeds maximum length");
  }
  binary::writeStringBytes(bytes, offset, ownerTable);
  return bytes;
}

void PageDirectoryEntry::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != serializedSize()) {
    throw std::invalid_argument("Invalid page directory entry size");
  }
  std::size_t offset = 0;
  pageId = binary::readLittleEndian<PageId>(bytes, offset);
  const auto pageTypeValue = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  pageType = static_cast<PageType>(pageTypeValue);
  creationTimestampUnixNs = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  const auto statusValue = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  status = static_cast<PageStatus>(statusValue);
  const auto ownerLength = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (ownerLength > kMaxOwnerLength || offset + ownerLength > bytes.size()) {
    throw std::invalid_argument("Invalid page directory owner encoding");
  }
  ownerTable.assign(reinterpret_cast<const char*>(bytes.data() + offset), ownerLength);
}

void PageDirectory::registerPage(const PageDirectoryEntry& entry) {
  entries_[entry.pageId] = entry;
}

void PageDirectory::unregisterPage(PageId pageId) {
  entries_.erase(pageId);
}

const PageDirectoryEntry* PageDirectory::get(PageId pageId) const {
  const auto it = entries_.find(pageId);
  return it == entries_.end() ? nullptr : &it->second;
}

std::vector<PageDirectoryEntry> PageDirectory::entries() const {
  std::vector<PageDirectoryEntry> result;
  result.reserve(entries_.size());
  for (const auto& [pageId, entry] : entries_) {
    (void)pageId;
    result.push_back(entry);
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.pageId < b.pageId;
  });
  return result;
}

std::array<Byte, kDefaultPageSize - PageHeader::serializedSize()> PageDirectory::serialize() const {
  std::array<Byte, kDefaultPageSize - PageHeader::serializedSize()> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, format::kPageDirectoryFormatVersion);
  const auto count = static_cast<std::uint32_t>(entries_.size());
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, count);
  for (const auto& entry : entries()) {
    const auto entryBytes = entry.serialize();
    if (offset + entryBytes.size() > bytes.size()) {
      throw std::runtime_error("Page directory exceeds page payload");
    }
    binary::writeBytes(bytes, offset, entryBytes);
  }
  return bytes;
}

void PageDirectory::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != kDefaultPageSize - PageHeader::serializedSize()) {
    throw std::invalid_argument("Invalid page directory payload size");
  }
  std::size_t offset = 0;
  const auto version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (version != kMetadataVersion) {
    throw std::invalid_argument("Unsupported page directory version");
  }
  const auto count = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (count > (bytes.size() - format::kPageDirectoryEntriesOffset) / PageDirectoryEntry::serializedSize()) {
    throw std::invalid_argument("Page directory entry count exceeds payload capacity");
  }
  entries_.clear();
  std::unordered_set<PageId> seenPageIds;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (offset + PageDirectoryEntry::serializedSize() > bytes.size()) {
      throw std::invalid_argument("Truncated page directory");
    }
    PageDirectoryEntry entry;
    entry.deserialize(std::span<const Byte>(bytes.data() + offset, PageDirectoryEntry::serializedSize()));
    offset += PageDirectoryEntry::serializedSize();
    if (!seenPageIds.insert(entry.pageId).second) {
      throw std::invalid_argument("Duplicate page directory entry detected");
    }
    entries_.emplace(entry.pageId, std::move(entry));
  }
}

} // namespace zenthrildb
