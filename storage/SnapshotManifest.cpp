#include "storage/SnapshotManifest.hpp"

#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"
#include "wal/LogRecord.hpp"
#include "wal/LogPayload.hpp"
#include "storage/SafeFilePath.hpp"
#include "storage/StorageLimits.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace zenthrildb {
namespace {

constexpr auto kMaxPageCount = static_cast<std::uint64_t>(std::numeric_limits<PageId>::max()) + 1;
constexpr auto kMaxFileBytes = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());

void validateManifest(const SnapshotManifest& manifest) {
  if (manifest.pageCount < kFirstUserPageId || manifest.pageCount > kMaxPageCount ||
      manifest.pageCount > kMaxDatabasePages) {
    throw std::runtime_error("Snapshot manifest page count is out of bounds");
  }
  const auto expectedSize = DatabaseHeader::serializedSize() + manifest.pageCount * kDefaultPageSize;
  if (manifest.databaseBytes != expectedSize) {
    throw std::runtime_error("Snapshot manifest database size disagrees with page count");
  }
  if (manifest.walPrefixBytes < WalIdentity::kSize || manifest.walPrefixBytes > kMaxFileBytes) {
    throw std::runtime_error("Snapshot manifest WAL prefix size is out of bounds");
  }
  if (manifest.transactionHighWater == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("Snapshot manifest transaction ID high-water is invalid");
  }
  if (manifest.identity.reducedHistory) {
    if (manifest.walPrefixBytes != WalIdentity::kSize + LogRecord::kHeaderSize + LogPayloadCodec::kSafeCheckpointPayloadSize) {
      throw std::runtime_error("Reduced WAL manifest must cover exactly its boundary record");
    }
  } else if (manifest.identity.snapshotLsn == 0) {
    if (manifest.walPrefixBytes != WalIdentity::kSize || manifest.transactionHighWater != 0) {
      throw std::runtime_error("Snapshot manifest has a nonempty history at LSN zero");
    }
  } else if (manifest.walPrefixBytes < WalIdentity::kSize + LogRecord::kHeaderSize) {
    throw std::runtime_error("Snapshot manifest WAL prefix cannot contain its snapshot LSN");
  }
}

// Reflected IEEE CRC32, with the same polynomial and initial/final masks as CRCManager.
constexpr auto makeCrcTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    auto crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      const auto mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xEDB88320u & mask);
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kCrcTable = makeCrcTable();

} // namespace

std::array<Byte, SnapshotManifest::kSize> SnapshotManifest::serialize() const {
  validateManifest(*this);
  const auto identityBytes = identity.serialize();
  std::array<Byte, kSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian(bytes, offset, kMagic);
  binary::writeLittleEndian(bytes, offset, kVersion);
  binary::writeLittleEndian(bytes, offset, static_cast<std::uint32_t>(kSize));
  binary::writeBytes(bytes, offset, identityBytes);
  binary::writeLittleEndian(bytes, offset, transactionHighWater);
  binary::writeLittleEndian(bytes, offset, pageCount);
  binary::writeLittleEndian(bytes, offset, databaseBytes);
  binary::writeLittleEndian(bytes, offset, walPrefixBytes);
  binary::writeLittleEndian(bytes, offset, databaseCrc);
  binary::writeLittleEndian(bytes, offset, walPrefixCrc);
  const auto crc = CRCManager::compute(std::span<const Byte>(bytes).first(offset));
  binary::writeLittleEndian(bytes, offset, crc);
  return bytes;
}

SnapshotManifest SnapshotManifest::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != kSize) throw std::runtime_error("Invalid snapshot manifest size");

  std::size_t offset = 0;
  const auto magic = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto size = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (magic != kMagic || size != kSize) throw std::runtime_error("Invalid snapshot manifest header");
  if (version != kVersion) throw std::runtime_error("Unsupported snapshot manifest version");

  constexpr auto crcOffset = kSize - sizeof(std::uint32_t);
  auto checksumOffset = crcOffset;
  const auto crc = binary::readLittleEndian<std::uint32_t>(bytes, checksumOffset);
  if (!CRCManager::verify(bytes.first(crcOffset), crc)) {
    throw std::runtime_error("Snapshot manifest checksum verification failed");
  }

  SnapshotManifest manifest;
  manifest.identity = WalIdentity::deserialize(bytes.subspan(offset, WalIdentity::kSize));
  offset += WalIdentity::kSize;
  manifest.transactionHighWater = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  manifest.pageCount = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  manifest.databaseBytes = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  manifest.walPrefixBytes = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  manifest.databaseCrc = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  manifest.walPrefixCrc = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  validateManifest(manifest);
  return manifest;
}

std::filesystem::path snapshotManifestPath(const std::filesystem::path& database) {
  auto path = database;
  path += ".snapshot";
  return path;
}

std::filesystem::path snapshotBaselinePath(const std::filesystem::path& database) {
  auto path = database;
  path += ".baseline";
  return path;
}

SnapshotManifest readSnapshotManifest(const std::filesystem::path& path) {
  detail::validateStoragePath(path);
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("Snapshot manifest is missing or is not a regular file");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Failed to open snapshot manifest");
  std::array<Byte, SnapshotManifest::kSize> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Failed to read snapshot manifest");
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("Truncated snapshot manifest");
  }
  if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Snapshot manifest has trailing bytes");
  if (input.bad() || !input.eof()) throw std::runtime_error("Failed to verify snapshot manifest length");
  return SnapshotManifest::deserialize(bytes);
}

void writeSnapshotManifest(const std::filesystem::path& path, const SnapshotManifest& manifest) {
  const auto bytes = manifest.serialize();
  detail::validateStoragePath(path, true);
  if (!std::filesystem::exists(path)) detail::createStorageFileExclusive(path);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Failed to open snapshot manifest for writing");
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("Failed to write snapshot manifest");
  output.close();
  if (!output) throw std::runtime_error("Failed to close snapshot manifest");
}

std::uint32_t snapshotFileCrc(const std::filesystem::path& path, std::uint64_t bytes) {
  detail::validateStoragePath(path);
  if (bytes > kMaxFileBytes) throw std::runtime_error("Snapshot checksum byte count is out of bounds");
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("Snapshot checksum file is missing or is not a regular file");
  }
  if (std::filesystem::file_size(path) < bytes) {
    throw std::runtime_error("Snapshot checksum file is shorter than the declared size");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Failed to open file for snapshot checksum");

  std::array<Byte, 64 * 1024> buffer{};
  auto crc = std::uint32_t{0xFFFFFFFFu};
  while (bytes != 0) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, buffer.size()));
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
    if (input.bad() || (input.fail() && !input.eof())) {
      throw std::runtime_error("Failed to read file for snapshot checksum");
    }
    if (input.gcount() != static_cast<std::streamsize>(count)) {
      throw std::runtime_error("Snapshot checksum file became shorter than the declared size");
    }
    for (const auto byte : std::span<const Byte>(buffer).first(count)) {
      crc = (crc >> 8u) ^ kCrcTable[(crc ^ byte) & 0xFFu];
    }
    bytes -= count;
  }
  return ~crc;
}

} // namespace zenthrildb
