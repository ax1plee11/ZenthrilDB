#pragma once

#include "storage/PageHeader.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace zenthrildb {

class Page {
public:
  explicit Page(PageSize pageSize = kDefaultPageSize);

  [[nodiscard]] PageHeader& header() noexcept { return header_; }
  [[nodiscard]] const PageHeader& header() const noexcept { return header_; }
  [[nodiscard]] std::span<Byte> payload() noexcept;
  [[nodiscard]] std::span<const Byte> payload() const noexcept;
  [[nodiscard]] PageSize pageSize() const noexcept { return pageSize_; }

  void clear() noexcept;
  [[nodiscard]] std::array<Byte, kDefaultPageSize> serialize() const;
  void deserialize(std::span<const Byte> bytes);

private:
  PageSize pageSize_;
  PageHeader header_{};
  std::array<Byte, kDefaultPageSize> data_{};
};

} // namespace zenthrildb
