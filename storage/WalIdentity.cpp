#include "storage/WalIdentity.hpp"

#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"

#include <istream>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace zenthrildb {
namespace {
bool isZero(std::span<const Byte> bytes) {
  return std::ranges::all_of(bytes, [](Byte value) { return value == 0; });
}
} // namespace

std::array<Byte, WalIdentity::kSize> WalIdentity::serialize() const {
  if (isZero(generation) || isZero(databaseUuid) || snapshotLsn == std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("Invalid WAL identity values");
  }
  if (reducedHistory && (!baselineRequired || snapshotLsn >= std::numeric_limits<std::uint64_t>::max() - 1)) {
    throw std::invalid_argument("Reduced WAL requires a baseline and space for its boundary record");
  }
  std::array<Byte, kSize> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian(bytes, offset, kMagic);
  const auto version = reducedHistory ? kReducedHistoryVersion : (baselineRequired ? kBaselineRequiredVersion : kVersion);
  binary::writeLittleEndian(bytes, offset, version);
  binary::writeLittleEndian(bytes, offset, static_cast<std::uint32_t>(kSize));
  binary::writeBytes(bytes, offset, generation);
  // Version 1 retains the reserved zero. Version 2 requires the baseline flag.
  binary::writeLittleEndian(bytes, offset, reducedHistory ? 3u : (baselineRequired ? 1u : 0u));
  binary::writeBytes(bytes, offset, databaseUuid);
  binary::writeLittleEndian(bytes, offset, snapshotLsn);
  const auto checksum = CRCManager::compute(std::span<const Byte>(bytes).first(offset));
  binary::writeLittleEndian(bytes, offset, checksum);
  return bytes;
}

WalIdentity WalIdentity::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != kSize) throw std::runtime_error("Truncated WAL identity header");
  std::size_t offset = 0;
  const auto magic = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto version = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto size = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (magic != kMagic || (version != kVersion && version != kBaselineRequiredVersion &&
      version != kReducedHistoryVersion) || size != kSize) {
    throw std::runtime_error("Unsupported or invalid WAL identity header");
  }
  WalIdentity identity;
  binary::readBytes(bytes, offset, identity.generation);
  const auto reserved = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  identity.baselineRequired = version >= kBaselineRequiredVersion;
  identity.reducedHistory = version == kReducedHistoryVersion;
  binary::readBytes(bytes, offset, identity.databaseUuid);
  identity.snapshotLsn = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  const auto checksum = CRCManager::compute(bytes.first(offset));
  if (binary::readLittleEndian<std::uint32_t>(bytes, offset) != checksum ||
      reserved != (identity.reducedHistory ? 3u : (identity.baselineRequired ? 1u : 0u)) ||
      isZero(identity.generation) || isZero(identity.databaseUuid) ||
      identity.snapshotLsn == std::numeric_limits<std::uint64_t>::max() ||
      (identity.reducedHistory && identity.snapshotLsn >= std::numeric_limits<std::uint64_t>::max() - 1)) {
    throw std::runtime_error("Corrupted WAL identity header");
  }
  return identity;
}

WalIdentity WalIdentity::fresh(const DatabaseHeader& header, std::uint64_t snapshotLsn) {
  WalIdentity identity{.generation = newGeneration(), .databaseUuid = header.databaseUuid, .snapshotLsn = snapshotLsn};
  (void)identity.serialize();
  return identity;
}

std::array<Byte, 16> WalIdentity::newGeneration() {
  std::array<Byte, 16> generation{};
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, generation.data(), static_cast<ULONG>(generation.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("Failed to obtain snapshot generation from the system RNG");
  }
#else
  std::size_t filled = 0;
  while (filled < generation.size()) {
    const auto count = ::getrandom(generation.data() + filled, generation.size() - filled, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Failed to generate snapshot identity");
    filled += static_cast<std::size_t>(count);
  }
#endif
  if (isZero(generation)) throw std::runtime_error("System RNG produced an invalid generation");
  return generation;
}

std::optional<WalIdentity> readWalIdentity(std::istream& stream) {
  std::array<Byte, WalIdentity::kSize> bytes{};
  stream.read(reinterpret_cast<char*>(bytes.data()), 4);
  if (stream.bad() || (stream.fail() && !stream.eof())) throw std::runtime_error("Failed to read WAL identity");
  if (stream.gcount() > 0 && stream.gcount() < 4) {
    std::array<Byte, 4> magic{};
    std::size_t magicOffset = 0;
    binary::writeLittleEndian(magic, magicOffset, WalIdentity::kMagic);
    if (std::equal(bytes.begin(), bytes.begin() + stream.gcount(), magic.begin())) {
      throw std::runtime_error("Truncated WAL identity header");
    }
  }
  std::size_t offset = 0;
  if (stream.gcount() != 4 || binary::readLittleEndian<std::uint32_t>(bytes, offset) != WalIdentity::kMagic) {
    stream.clear();
    stream.seekg(0);
    if (!stream) throw std::runtime_error("Failed to seek to WAL start");
    return std::nullopt;
  }
  stream.read(reinterpret_cast<char*>(bytes.data() + 4), static_cast<std::streamsize>(bytes.size() - 4));
  if (stream.gcount() != static_cast<std::streamsize>(bytes.size() - 4)) {
    throw std::runtime_error("Truncated WAL identity header");
  }
  return WalIdentity::deserialize(bytes);
}

Page WalIdentity::toPage() const {
  Page page;
  page.header().pageId = kHeaderMirrorPageId;
  page.header().pageType = PageType::Metadata;
  page.header().version = Page::kFormatVersionV2;
  page.header().usedBytes = kSize;
  std::size_t offset = 0;
  binary::writeBytes(page.payload(), offset, serialize());
  return page;
}

} // namespace zenthrildb
