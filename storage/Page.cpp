#include "storage/Page.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace zenthrildb {

Page::Page(PageSize pageSize)
  : pageSize_(pageSize) {
  if (pageSize_ != kDefaultPageSize) {
    throw std::invalid_argument("ZenthrilDB MVP only supports 8192-byte pages");
  }
}

std::span<Byte> Page::payload() noexcept {
  return std::span<Byte>(data_).subspan(PageHeader::serializedSize(), pageSize_ - PageHeader::serializedSize());
}

std::span<const Byte> Page::payload() const noexcept {
  return std::span<const Byte>(data_).subspan(PageHeader::serializedSize(), pageSize_ - PageHeader::serializedSize());
}

void Page::clear() noexcept {
  data_.fill(0);
  header_ = {};
}

std::array<Byte, kDefaultPageSize> Page::serialize() const {
  auto output = std::array<Byte, kDefaultPageSize>{};
  const auto headerBytes = header_.serialize();
  std::copy(headerBytes.begin(), headerBytes.end(), output.begin());
  std::copy(data_.begin() + static_cast<std::ptrdiff_t>(PageHeader::serializedSize()),
            data_.end(),
            output.begin() + static_cast<std::ptrdiff_t>(PageHeader::serializedSize()));
  return output;
}

void Page::deserialize(std::span<const Byte> bytes) {
  if (bytes.size() != pageSize_) {
    throw std::invalid_argument("Invalid page size during deserialization");
  }
  std::copy(bytes.begin(), bytes.end(), data_.begin());
  header_.deserialize(std::span<const Byte>(data_.data(), PageHeader::serializedSize()));
}

} // namespace zenthrildb
