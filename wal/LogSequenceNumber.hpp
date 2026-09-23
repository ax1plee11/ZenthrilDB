#pragma once

#include <cstdint>

namespace zenthrildb {

class LogSequenceNumber {
public:
  constexpr LogSequenceNumber() = default;
  explicit constexpr LogSequenceNumber(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr LogSequenceNumber next() const noexcept { return LogSequenceNumber{value_ + 1}; }

  friend constexpr bool operator==(LogSequenceNumber lhs, LogSequenceNumber rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr bool operator<(LogSequenceNumber lhs, LogSequenceNumber rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

private:
  std::uint64_t value_{0};
};

} // namespace zenthrildb
