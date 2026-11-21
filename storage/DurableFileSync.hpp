#pragma once

#include <filesystem>
#include <system_error>
#include "storage/SafeFilePath.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zenthrildb::detail {

inline void syncFileToStorage(const std::filesystem::path& path) {
  validateStoragePath(path);
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "Failed to open file for durable flush");
  }
  const auto flushed = FlushFileBuffers(handle);
  const auto error = flushed ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!flushed) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "Failed to durably flush file");
  }
#else
  const auto descriptor = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor == -1) {
    throw std::system_error(errno, std::generic_category(), "Failed to open file for durable flush");
  }
  const auto flushed = ::fsync(descriptor);
  const auto error = errno;
  ::close(descriptor);
  if (flushed == -1) {
    throw std::system_error(error, std::generic_category(), "Failed to durably flush file");
  }
#endif
}

inline void syncDirectoryToStorage(const std::filesystem::path& directory) {
#ifdef _WIN32
  (void)directory; // Replacement uses MOVEFILE_WRITE_THROUGH on Windows.
#else
  const auto target = directory.empty() ? std::filesystem::path(".") : directory;
  const auto descriptor = ::open(target.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor == -1) {
    throw std::system_error(errno, std::generic_category(), "Failed to open directory for durable flush");
  }
  const auto flushed = ::fsync(descriptor);
  const auto error = errno;
  ::close(descriptor);
  if (flushed == -1) {
    throw std::system_error(error, std::generic_category(), "Failed to durably flush directory");
  }
#endif
}

} // namespace zenthrildb::detail
