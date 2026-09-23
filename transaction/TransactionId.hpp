#pragma once

#include <cstdint>
#include <functional>

namespace zenthrildb {

class TransactionId {
public:
  constexpr TransactionId() = default;
  explicit constexpr TransactionId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(TransactionId lhs, TransactionId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr auto operator<=>(TransactionId lhs, TransactionId rhs) noexcept = default;

private:
  std::uint64_t value_{0};
};

} // namespace zenthrildb

template <>
struct std::hash<zenthrildb::TransactionId> {
  [[nodiscard]] std::size_t operator()(zenthrildb::TransactionId id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};
