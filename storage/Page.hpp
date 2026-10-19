#pragma once

#include "storage/PageHeader.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace zenthrildb {

class Page {
public:
  static constexpr std::uint32_t kFormatVersionV1 = 1;
  static constexpr std::uint32_t kFormatVersionV2 = 2;
  static constexpr std::size_t kV1PayloadSize = kDefaultPageSize - PageHeader::kSerializedSize;
  static constexpr std::size_t kV2PageLsnOffset = kDefaultPageSize - sizeof(std::uint64_t);
  static constexpr std::size_t kV2PayloadSize = kV2PageLsnOffset - PageHeader::kSerializedSize;

  explicit Page(PageSize pageSize = kDefaultPageSize);

  [[nodiscard]] PageHeader& header() noexcept { return header_; }
  [[nodiscard]] const PageHeader& header() const noexcept { return header_; }
  [[nodiscard]] std::span<Byte> payload() noexcept;
  [[nodiscard]] std::span<const Byte> payload() const noexcept;
  [[nodiscard]] PageSize pageSize() const noexcept { return pageSize_; }
  [[nodiscard]] std::uint64_t pageLsn() const;
  void setPageLsn(std::uint64_t lsn);

  void clear() noexcept;
  [[nodiscard]] std::array<Byte, kDefaultPageSize> serialize() const;
  void deserialize(std::span<const Byte> bytes);

private:
  PageSize pageSize_;
  PageHeader header_{};
  std::array<Byte, kDefaultPageSize> data_{};
  std::uint64_t pageLsn_{0};
};

} // namespace zenthrildb
