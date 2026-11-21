#include "storage/WalInstallation.hpp"
#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"
#include "storage/DurableFileSync.hpp"
#include "storage/IoFaultInjector.hpp"
#include "storage/SafeFilePath.hpp"
#include <fstream>
#include <iterator>
#include <stdexcept>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zenthrildb {
namespace {
constexpr std::uint32_t magic = 0x5349445a; // ZDIS
constexpr std::size_t fixedSize = 160;
void validate(const WalInstallation& state) {
  const auto encoded = state.targetPath.u8string();
  if (!state.targetPath.is_absolute() || state.targetPath.extension() != ".zdb" ||
      state.targetPath.lexically_normal() != state.targetPath || encoded.empty() || encoded.size() > 4096 ||
      encoded.find(u8'\0') != std::u8string::npos || !state.target.reducedHistory ||
      state.source.databaseUuid != state.target.databaseUuid || state.source.generation == state.target.generation ||
      state.sourceDataBytes < DatabaseHeader::kSerializedSize + 4 * kDefaultPageSize ||
      state.sourceWalBytes < WalIdentity::kSize) {
    throw std::runtime_error("Invalid WAL installation selector");
  }
}
}

std::filesystem::path walInstallationPath(const std::filesystem::path& database) {
  auto path = database;
  path += ".install";
  return path;
}

std::vector<Byte> WalInstallation::serialize() const {
  validate(*this);
  const auto path = targetPath.u8string();
  std::vector<Byte> bytes(fixedSize + path.size());
  std::size_t offset = 0;
  binary::writeLittleEndian(bytes, offset, magic);
  binary::writeLittleEndian(bytes, offset, 1u);
  binary::writeLittleEndian(bytes, offset, 1u); // Only COMMITTED exists on disk.
  binary::writeBytes(bytes, offset, source.serialize());
  binary::writeBytes(bytes, offset, target.serialize());
  binary::writeLittleEndian(bytes, offset, sourceDataBytes);
  binary::writeLittleEndian(bytes, offset, sourceWalBytes);
  binary::writeLittleEndian(bytes, offset, sourceHasBaseline ? 1u : 0u);
  binary::writeLittleEndian(bytes, offset, static_cast<std::uint32_t>(path.size()));
  binary::writeBytes(bytes, offset, {reinterpret_cast<const Byte*>(path.data()), path.size()});
  binary::writeLittleEndian(bytes, offset, CRCManager::compute(std::span<const Byte>(bytes).first(offset)));
  return bytes;
}

WalInstallation WalInstallation::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() < fixedSize || bytes.size() > fixedSize + 4096) throw std::runtime_error("Installation selector size is invalid");
  auto crcOffset = bytes.size() - 4;
  auto crcRead = crcOffset;
  if (!CRCManager::verify(bytes.first(crcOffset), binary::readLittleEndian<std::uint32_t>(bytes, crcRead))) {
    throw std::runtime_error("Installation selector CRC mismatch");
  }
  std::size_t offset = 0;
  if (binary::readLittleEndian<std::uint32_t>(bytes, offset) != magic ||
      binary::readLittleEndian<std::uint32_t>(bytes, offset) != 1 ||
      binary::readLittleEndian<std::uint32_t>(bytes, offset) != 1) throw std::runtime_error("Unsupported installation selector");
  WalInstallation state;
  state.source = WalIdentity::deserialize(bytes.subspan(offset, WalIdentity::kSize)); offset += WalIdentity::kSize;
  state.target = WalIdentity::deserialize(bytes.subspan(offset, WalIdentity::kSize)); offset += WalIdentity::kSize;
  state.sourceDataBytes = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  state.sourceWalBytes = binary::readLittleEndian<std::uint64_t>(bytes, offset);
  const auto baseline = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  const auto length = binary::readLittleEndian<std::uint32_t>(bytes, offset);
  if (baseline > 1 || length != bytes.size() - fixedSize) throw std::runtime_error("Invalid installation selector fields");
  state.sourceHasBaseline = baseline != 0;
  const auto text = bytes.subspan(offset, length);
  state.targetPath = std::filesystem::path(std::u8string(text.begin(), text.end()));
  validate(state);
  return state;
}

WalInstallation readWalInstallation(const std::filesystem::path& database) {
  const auto path = walInstallationPath(database);
  detail::validateStoragePath(path);
  if (std::filesystem::file_size(path) > fixedSize + 4096) throw std::runtime_error("Installation selector exceeds limit");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read installation selector");
  const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) throw std::runtime_error("Failed to read installation selector");
  return WalInstallation::deserialize(bytes);
}

void publishWalInstallation(const std::filesystem::path& database, const WalInstallation& state) {
  auto staging = walInstallationPath(database);
  const auto token = WalIdentity::newGeneration();
  staging += ".pending-";
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : token) { staging += hex[byte >> 4]; staging += hex[byte & 15]; }
  IoFaultInjector::failIfArmed(IoFaultPoint::NoSpace);
  detail::createStorageFileExclusive(staging);
  bool published = false;
  try {
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallCreated);
    const auto bytes = state.serialize();
    std::ofstream output(staging, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot open installation staging file");
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallWrite);
    if (IoFaultInjector::isArmed(IoFaultPoint::InstallPartial)) {
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size() / 2);
      output.close();
      IoFaultInjector::failIfArmed(IoFaultPoint::InstallPartial);
      throw std::runtime_error("Incomplete installation selector");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.flush();
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallFlush);
    output.close();
    if (!output) throw std::runtime_error("Failed to write installation selector");
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallSync);
    detail::syncFileToStorage(staging);
    {
      std::ifstream input(staging, std::ios::binary);
      if (!input) throw std::runtime_error("Cannot verify staged installation selector");
      const std::vector<Byte> saved{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      if (input.bad() || WalInstallation::deserialize(saved).serialize() != bytes) {
        throw std::runtime_error("Staged installation selector verification failed");
      }
    }
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallDurable);
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallRename);
    const auto destination = walInstallationPath(database);
#ifdef _WIN32
    if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot publish installation selector");
    }
#else
    if (::renameat2(AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) != 0) {
      throw std::system_error(errno, std::generic_category(), "Cannot publish installation selector");
    }
#endif
    published = true;
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallSwitched);
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallDirectorySync);
    detail::syncDirectoryToStorage(database.parent_path());
    IoFaultInjector::failIfArmed(IoFaultPoint::InstallParentDurable);
  } catch (...) {
    if (!published) {
      // Cleanup failure cannot hide the original failure or select staging.
      try {
        IoFaultInjector::failIfArmed(IoFaultPoint::InstallCleanup);
        std::filesystem::remove(staging);
      } catch (...) { }
    }
    throw;
  }
}
}
