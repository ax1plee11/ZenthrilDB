#pragma once

#include "core/Types.hpp"
#include "transaction/TransactionId.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>

namespace zenthrildb {

enum class LockMode : std::uint32_t {
  Shared = 1,
  Exclusive = 2,
};

enum class LockResourceType : std::uint32_t {
  Database = 1,
  Metadata = 2,
  Table = 3,
  Page = 4,
};

// LockUpgradePolicy controls deadlock prevention behavior for lock upgrades.
// WEAKNESS FIXED: lock upgrades had no policy and could deadlock.
enum class LockUpgradePolicy : std::uint32_t {
  Wait = 0,        // Wait for other Shared holders to release.
  Fail = 1,        // Fail immediately if upgrade is not possible.
  PreAllocate = 2, // Reserve Exclusive lock at Shared acquisition (two-phase).
};

// DeadlockVictimStrategy controls which transaction is chosen as deadlock victim.
// WEAKNESS FIXED: no victim selection strategy existed.
enum class DeadlockVictimStrategy : std::uint32_t {
  Oldest = 0,     // Victimize the oldest transaction (least costly to rollback).
  Youngest = 1,   // Victimize the youngest transaction.
  FewestLocks = 2, // Victimize the transaction holding the fewest locks.
  MinimumWait = 3, // Victimize the transaction with the minimum total wait time.
};

struct LockResourceId {
  LockResourceType type{LockResourceType::Database};
  std::uint64_t value{0};

  [[nodiscard]] static constexpr LockResourceId database() noexcept {
    return LockResourceId{.type = LockResourceType::Database, .value = 0};
  }

  [[nodiscard]] static constexpr LockResourceId metadata(std::uint64_t id) noexcept {
    return LockResourceId{.type = LockResourceType::Metadata, .value = id};
  }

  [[nodiscard]] static constexpr LockResourceId table(std::uint64_t tableId) noexcept {
    return LockResourceId{.type = LockResourceType::Table, .value = tableId};
  }

  [[nodiscard]] static constexpr LockResourceId page(PageId pageId) noexcept {
    return LockResourceId{.type = LockResourceType::Page, .value = pageId};
  }

  friend constexpr bool operator==(LockResourceId lhs, LockResourceId rhs) noexcept {
    return lhs.type == rhs.type && lhs.value == rhs.value;
  }

  friend constexpr auto operator<=>(LockResourceId lhs, LockResourceId rhs) noexcept = default;
};

struct LockRequest {
  TransactionId transactionId{};
  LockResourceId resource{};
  LockMode mode{LockMode::Shared};
  LockUpgradePolicy upgradePolicy{LockUpgradePolicy::Wait};
};

struct LockRequestResult {
  bool acquired{false};
  bool timedOut{false};
  bool upgraded{false};
  bool deadlockDetected{false};
  std::optional<TransactionId> deadlockVictim{};
};

} // namespace zenthrildb

template <>
struct std::hash<zenthrildb::LockResourceId> {
  [[nodiscard]] std::size_t operator()(zenthrildb::LockResourceId id) const noexcept {
    const auto type = static_cast<std::uint64_t>(id.type);
    return std::hash<std::uint64_t>{}((type << 56u) ^ id.value);
  }
};
