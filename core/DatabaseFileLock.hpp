#pragma once

#include <filesystem>
#include <stdexcept>

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

// An exclusive lock held for the lifetime of an open FileManager.
// Keep the sidecar file: unlinking it could split the lock domain.
class DatabaseFileLock final {
public:
  explicit DatabaseFileLock(const std::filesystem::path& databasePath) {
    auto lockPath = databasePath;
    lockPath += ".lock";
#ifdef _WIN32
    handle_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open database lock file");
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &overlapped_)) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("Database is already open by another process");
    }
#else
    fd_ = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) throw std::runtime_error("Cannot open database lock file");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("Database is already open by another process");
    }
#endif
  }

  ~DatabaseFileLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
#endif
  }

  DatabaseFileLock(const DatabaseFileLock&) = delete;
  DatabaseFileLock& operator=(const DatabaseFileLock&) = delete;

private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
  OVERLAPPED overlapped_{};
#else
  int fd_{-1};
#endif
};

} // namespace zenthrildb
