#pragma once

#include "core/Types.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace zenthrildb::binary {

template <std::unsigned_integral T>
void writeLittleEndian(std::span<Byte> bytes, std::size_t& offset, T value) {
  if (offset + sizeof(T) > bytes.size()) {
    throw std::out_of_range("Binary write exceeds buffer size");
  }
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes[offset + i] = static_cast<Byte>((value >> (i * 8u)) & static_cast<T>(0xFFu));
  }
  offset += sizeof(T);
}

template <std::unsigned_integral T>
void appendLittleEndian(std::vector<Byte>& bytes, T value) {
  const auto oldSize = bytes.size();
  bytes.resize(oldSize + sizeof(T));
  auto offset = oldSize;
  writeLittleEndian<T>(std::span<Byte>(bytes.data(), bytes.size()), offset, value);
}

template <std::unsigned_integral T>
[[nodiscard]] T readLittleEndian(std::span<const Byte> bytes, std::size_t& offset) {
  if (offset + sizeof(T) > bytes.size()) {
    throw std::out_of_range("Binary read exceeds buffer size");
  }
  T value{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(bytes[offset + i]) << (i * 8u);
  }
  offset += sizeof(T);
  return value;
}

inline void writeBytes(std::span<Byte> bytes, std::size_t& offset, std::span<const Byte> source) {
  if (offset + source.size() > bytes.size()) {
    throw std::out_of_range("Binary byte write exceeds buffer size");
  }
  std::copy(source.begin(), source.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += source.size();
}

inline void readBytes(std::span<const Byte> bytes, std::size_t& offset, std::span<Byte> destination) {
  if (offset + destination.size() > bytes.size()) {
    throw std::out_of_range("Binary byte read exceeds buffer size");
  }
  std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + destination.size()),
            destination.begin());
  offset += destination.size();
}

inline void writeStringBytes(std::span<Byte> bytes, std::size_t& offset, const std::string& value) {
  writeBytes(bytes, offset, std::span<const Byte>(
                            reinterpret_cast<const Byte*>(value.data()), value.size()));
}

} // namespace zenthrildb::binary
