#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace zenthrildb {

class TableMetadata {
public:
  static constexpr std::size_t kMaxNameLength = 64;
  static constexpr std::uint32_t kFormatVersion = 1;
  static constexpr std::size_t kSerializedSize = 128;

  TableMetadata() = default;
  TableMetadata(std::uint64_t tableId, std::string name, PageId rootPageId);

  [[nodiscard]] std::uint64_t tableId() const noexcept { return tableId_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] PageId rootPageId() const noexcept { return rootPageId_; }
  [[nodiscard]] std::uint64_t creationTimestampUnixNs() const noexcept { return creationTimestampUnixNs_; }
  [[nodiscard]] std::uint64_t recordCount() const noexcept { return recordCount_; }
  [[nodiscard]] std::uint64_t pageCount() const noexcept { return pageCount_; }
  [[nodiscard]] std::uint32_t formatVersion() const noexcept { return formatVersion_; }
  [[nodiscard]] std::uint32_t tableVersion() const noexcept { return tableVersion_; }

  void setRootPageId(PageId pageId) noexcept { rootPageId_ = pageId; }
  void setRecordCount(std::uint64_t count) noexcept { recordCount_ = count; }
  void setPageCount(std::uint64_t count) noexcept { pageCount_ = count; }
  void rename(std::string name);

  [[nodiscard]] std::array<Byte, kSerializedSize> serialize() const noexcept;
  void deserialize(std::span<const Byte> bytes);

private:
  std::uint32_t formatVersion_{kFormatVersion};
  std::uint64_t tableId_{0};
  std::string name_{};
  PageId rootPageId_{0};
  std::uint64_t creationTimestampUnixNs_{0};
  std::uint64_t recordCount_{0};
  std::uint64_t pageCount_{0};
  std::uint32_t tableVersion_{1};
};

} // namespace zenthrildb
