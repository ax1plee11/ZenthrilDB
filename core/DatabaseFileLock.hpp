#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cerrno>
#include "storage/IoFaultInjector.hpp"
#include "storage/SafeFilePath.hpp"
#include "storage/StorageLimits.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace zenthrildb {
class DatabaseBusyError final : public std::runtime_error {
public:
  DatabaseBusyError() : std::runtime_error("Database is already open by another process") {}
};

// An exclusive lock held for the lifetime of an open FileManager.
// Keep the sidecar file: unlinking it could split the lock domain.
class DatabaseFileLock final {
public:
  explicit DatabaseFileLock(const std::filesystem::path& databasePath, bool databaseDomain = true) {
    databasePath_ = std::filesystem::absolute(databasePath).lexically_normal();
    IoFaultInjector::failIfArmed(IoFaultPoint::OwnershipAcquire);
    detail::validateStoragePath(databasePath, true);
    if (databaseDomain) {
      checkStorageSize(databasePath, kMaxDatabaseBytes);
      auto walPath = databasePath;
      walPath.replace_extension(".wal");
      detail::validateStoragePath(walPath, true);
      checkStorageSize(walPath, kMaxWalBytes);
      for (const auto* suffix : {".baseline", ".snapshot"}) {
        auto sidecar = databasePath;
        sidecar += suffix;
        detail::validateStoragePath(sidecar, true);
        checkStorageSize(sidecar, suffix == std::string_view(".baseline") ? kMaxDatabaseBytes : 4096);
      }
    }
    auto lockPath = databasePath;
    lockPath += ".lock";
    detail::validateStoragePath(lockPath, true);
#ifdef _WIN32
    handle_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot open database lock file");
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &overlapped_)) {
      const auto error = GetLastError();
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      if (error == ERROR_LOCK_VIOLATION) throw DatabaseBusyError();
      throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot acquire database ownership");
    }
#else
    fd_ = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "Cannot open database lock file");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto error = errno;
      ::close(fd_);
      fd_ = -1;
      if (error == EWOULDBLOCK || error == EAGAIN) throw DatabaseBusyError();
      throw std::system_error(error, std::generic_category(), "Cannot acquire database ownership");
    }
#endif
    try { IoFaultInjector::failIfArmed(IoFaultPoint::OwnershipAcquired); }
    catch (...) { releaseStorage(); throw; }
  }

  ~DatabaseFileLock() {
    try { release(); }
    catch (...) { std::fputs("ZenthrilDB: database ownership release failed\n", stderr); }
  }

private:
  friend class WriteAheadLog;
  [[nodiscard]] std::unique_lock<std::recursive_mutex> cleanupGuard() const {
    std::unique_lock lock(operationMutex_, std::try_to_lock);
    if (!lock) throw std::logic_error("Cannot close WAL during an active lifecycle operation");
    return lock;
  }
  [[nodiscard]] std::unique_lock<std::recursive_mutex> guard() const {
    std::unique_lock lock(operationMutex_, std::try_to_lock);
    if (!lock) throw std::logic_error("Database lifecycle operation is busy");
    if (!active_) throw std::logic_error("Database ownership has been revoked");
#ifndef _WIN32
    if (ownerPid_ != ::getpid()) throw std::logic_error("Inherited database ownership cannot be used after fork");
#endif
    return lock;
  }
public:
  [[nodiscard]] const std::filesystem::path& databasePath() const noexcept { return databasePath_; }
  class Operation {
  public:
    explicit Operation(std::shared_ptr<DatabaseFileLock> owner)
      : owner_(std::move(owner)), lock_(owner_->guard()) {}
  private:
    std::shared_ptr<DatabaseFileLock> owner_;
    std::unique_lock<std::recursive_mutex> lock_;
  };
  void claimBuffer() {
    auto lock = guard();
    if (bufferAttached_) throw std::logic_error("Database already has a buffer cache");
    bufferAttached_ = true;
  }
  void releaseBuffer() noexcept {
    std::lock_guard lock(operationMutex_);
    bufferAttached_ = false;
  }

  DatabaseFileLock(const DatabaseFileLock&) = delete;
  DatabaseFileLock& operator=(const DatabaseFileLock&) = delete;
  DatabaseFileLock(DatabaseFileLock&&) = delete;
  DatabaseFileLock& operator=(DatabaseFileLock&&) = delete;

private:
  friend class FileManager;
  void invalidate() {
    std::lock_guard lock(operationMutex_);
    active_ = false;
  }
  void release() {
    std::lock_guard lock(operationMutex_);
    releaseStorage();
  }
  void releaseStorage() {
    active_ = false;
    std::error_code failure;
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      if (!UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_)) failure = {static_cast<int>(GetLastError()), std::system_category()};
      if (!CloseHandle(handle_) && !failure) failure = {static_cast<int>(GetLastError()), std::system_category()};
      handle_ = INVALID_HANDLE_VALUE;
      IoFaultInjector::failIfArmed(IoFaultPoint::OwnershipRelease);
    }
#else
    if (fd_ >= 0) {
      if (ownerPid_ == ::getpid() && ::flock(fd_, LOCK_UN) != 0) failure = {errno, std::generic_category()};
      if (::close(fd_) != 0 && !failure) failure = {errno, std::generic_category()};
      fd_ = -1;
      IoFaultInjector::failIfArmed(IoFaultPoint::OwnershipRelease);
    }
#endif
    if (failure) throw std::system_error(failure, "Operating system failed to release database ownership");
  }
  std::filesystem::path databasePath_;
  mutable std::recursive_mutex operationMutex_;
  bool active_{true};
  bool bufferAttached_{false};
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
  OVERLAPPED overlapped_{};
#else
  int fd_{-1};
  const pid_t ownerPid_{::getpid()};
#endif
};

} // namespace zenthrildb
