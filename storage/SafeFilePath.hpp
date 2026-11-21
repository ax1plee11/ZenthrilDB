#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <algorithm>
#include <cwctype>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zenthrildb::detail {

// Preflight for trusted, locally owned directories. This does not replace
// directory ownership: std::fstream still reopens paths after inspection.
inline void validateStoragePath(const std::filesystem::path& path, bool allowMissing = false,
                                bool directory = false) {
  if (path.empty() || (!directory && path.filename().empty())) throw std::runtime_error("Invalid storage path");
  auto absolute = std::filesystem::absolute(path);
  if (directory) {
    while (absolute.filename().empty() && absolute.has_relative_path()) absolute = absolute.parent_path();
  }
#ifdef _WIN32
  if (absolute.native().starts_with(L"\\\\")) throw std::runtime_error("Only local storage paths are supported");
#endif
  auto current = absolute.root_path();
  for (const auto& part : absolute.relative_path()) {
#ifdef _WIN32
    const auto name = part.native();
    auto stem = name.substr(0, name.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t ch) { return std::towupper(ch); });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
         stem[3] >= L'1' && stem[3] <= L'9')) throw std::runtime_error("Reserved Windows device path");
    if (name.find(L':') != std::wstring::npos ||
        (name != L"." && name != L".." && (name.back() == L'.' || name.back() == L' '))) {
      throw std::runtime_error("Ambiguous Windows storage path");
    }
#endif
    current /= part;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::system_error(error, "Cannot inspect storage path");
    }
    if (!std::filesystem::exists(status)) {
      if (allowMissing && current == absolute) return;
      throw std::filesystem::filesystem_error("Storage path component is missing", current,
        std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (std::filesystem::is_symlink(status)) throw std::runtime_error("Storage links are not supported");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      throw std::runtime_error("Storage reparse points are not supported");
    }
#endif
    if (current != absolute && !std::filesystem::is_directory(status)) {
      throw std::runtime_error("Storage parent is not a directory");
    }
    if (current == absolute) {
      if (directory) {
        if (!std::filesystem::is_directory(status)) throw std::runtime_error("Storage parent is not a directory");
        return;
      }
      if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("Storage must be a regular file");
#ifdef _WIN32
      // A DOS 8.3 filename would create a different sidecar for the same file.
      // Parent aliases retain the same sidecar inode and need no new lock domain.
      const auto required = GetLongPathNameW(current.c_str(), nullptr, 0);
      if (!required) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot validate long storage filename");
      std::wstring longPath(required, L'\0');
      const auto length = GetLongPathNameW(current.c_str(), longPath.data(), required);
      if (!length || length >= required) throw std::runtime_error("Cannot resolve long storage filename");
      longPath.resize(length);
      const auto filename = std::filesystem::path(longPath).filename().native();
      const auto requested = current.filename().native();
      if (CompareStringOrdinal(filename.c_str(), -1, requested.c_str(), -1, TRUE) != CSTR_EQUAL) {
        throw std::runtime_error("Short storage filename aliases are not supported");
      }
#endif
      if (std::filesystem::hard_link_count(current) != 1) {
        throw std::runtime_error("Storage hard links are not supported");
      }
    }
  }
}

inline void createStorageFileExclusive(const std::filesystem::path& path) {
  validateStoragePath(path, true);
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Storage file already exists or cannot be created");
  if (!CloseHandle(handle)) throw std::runtime_error("Failed to close new storage file");
#else
  const auto fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "Cannot exclusively create storage file");
  if (::close(fd) != 0) throw std::runtime_error("Failed to close new storage file");
#endif
}

} // namespace zenthrildb::detail
