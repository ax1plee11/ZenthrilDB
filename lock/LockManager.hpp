#pragma once

#include "lock/LockTypes.hpp"
#include "lock/WaitForGraph.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zenthrildb {

// DeadlockHandler is invoked when a deadlock is detected.
// WEAKNESS FIXED: previously deadlocks could only be resolved by timeout.
// ARCHITECTURE: LockManager must NOT own transaction lifecycle;
// it calls this handler so TransactionManager decides abort/rollback.
class DeadlockHandler {
public:
  virtual ~DeadlockHandler() = default;

  // OnDeadlock is called when deadlock is detected.
  // The handler receives the victim and all involved transactions.
  // Returns true if the victim was aborted, false if detection should retry.
  virtual bool OnDeadlock(TransactionId victim, const std::vector<TransactionId>& involved) = 0;
};

// LockWaitStats provides diagnostics for lock wait behavior.
// WEAKNESS FIXED: no diagnostics existed previously.
struct LockWaitStats {
  std::size_t totalWaits{0};
  std::size_t timeouts{0};
  std::size_t deadlocksDetected{0};
  std::size_t deadlocksResolved{0};
  std::size_t maxWaitQueueLength{0};
  std::size_t currentWaiters{0};
  std::chrono::nanoseconds maxWaitTime{0};
  std::chrono::nanoseconds avgWaitTime{0};
};

// WaiterEntry represents a transaction waiting for a lock.
// ARCHITECTURE: FIFO queue entry with timestamp for fairness.
struct WaiterEntry {
  TransactionId transactionId{};
  LockMode mode{LockMode::Shared};
  std::chrono::steady_clock::time_point queuedAt{};
  bool isUpgrade{false};  // upgrading from Shared to Exclusive
  std::condition_variable* cv{nullptr};
  bool granted{false};
  bool timedOut{false};
  std::size_t queuePosition{0};
};

// GrantedLock represents a lock that has been granted to a transaction.
struct GrantedLock {
  TransactionId transactionId{};
  LockMode mode{LockMode::Shared};
};

// LockResourceEntry holds granted locks and the FIFO wait queue for a resource.
struct LockResourceEntry {
  std::vector<GrantedLock> granted;
  std::list<WaiterEntry> waiters;  // FIFO queue — SECURITY: prevents thundering herd
};

class LockGuard;

class LockManager {
public:
  using Timeout = std::chrono::milliseconds;

  explicit LockManager(std::unique_ptr<DeadlockHandler> deadlockHandler = nullptr);
  ~LockManager();

  LockManager(const LockManager&) = delete;
  LockManager& operator=(const LockManager&) = delete;

  // Original API (preserved for compatibility)
  [[nodiscard]] bool tryAcquire(TransactionId transactionId, LockResourceId resource, LockMode mode);
  [[nodiscard]] bool acquire(TransactionId transactionId,
                             LockResourceId resource,
                             LockMode mode,
                             Timeout timeout = Timeout{1000});
  [[nodiscard]] LockGuard acquireGuard(TransactionId transactionId,
                                       LockResourceId resource,
                                       LockMode mode,
                                       Timeout timeout = Timeout{1000});

  void release(TransactionId transactionId, LockResourceId resource);
  void releaseAll(TransactionId transactionId);

  [[nodiscard]] bool holds(TransactionId transactionId, LockResourceId resource, LockMode mode) const;
  [[nodiscard]] bool holds(TransactionId transactionId, LockResourceId resource) const;

  [[nodiscard]] std::size_t activeLockCount() const;
  [[nodiscard]] std::size_t activeLockCount(LockResourceId resource) const;

  // SECURITY: lock upgrade from Shared to Exclusive with proper queuing.
  // WEAKNESS FIXED: lock upgrades could cause deadlock without queue fairness.
  [[nodiscard]] bool upgrade(TransactionId transactionId, LockResourceId resource, Timeout timeout = Timeout{1000});

  // SECURITY: lock downgrade from Exclusive to Shared.
  // ARCHITECTURE: allows graceful reduction of lock granularity.
  [[nodiscard]] bool downgrade(TransactionId transactionId, LockResourceId resource);

  // Wait-for graph and deadlock detection
  // WEAKNESS FIXED: no deadlock detection existed.
  [[nodiscard]] std::vector<TransactionId> DetectDeadlocks() const;

  // Victim selection interface
  // WEAKNESS FIXED: no victim selection interface existed.
  [[nodiscard]] std::optional<TransactionId> SelectVictim(const std::vector<TransactionId>& cycle) const;

  // Diagnostics API
  [[nodiscard]] LockWaitStats GetWaitStats() const;
  [[nodiscard]] std::vector<LockResourceId> GetWaitingResources(TransactionId transactionId) const;
  [[nodiscard]] std::size_t GetWaitQueueLength(LockResourceId resource) const;

  // Clear all locks for a transaction (used during abort/rollback).
  void ClearTransactionState(TransactionId transactionId);

private:
  friend class WaitForGraph;
  friend class LockGuard;

  [[nodiscard]] static bool compatible(LockMode requested, LockMode existing) noexcept;

  // Check if a lock can be granted immediately (FIFO fairness aware).
  // SECURITY: prevents writer starvation by blocking new Shared requests
  // when an Exclusive waiter exists in the queue.
  [[nodiscard]] bool canGrant(TransactionId transactionId,
                              LockResourceId resource,
                              LockMode mode,
                              const LockResourceEntry& entry) const;

  // Grant a lock immediately (for granted locks).
  void grant(TransactionId transactionId, LockResourceId resource, LockMode mode);

  // Grant a lock from the waiter queue (FIFO).
  void grantFromWaiter(LockResourceEntry& entry, WaiterEntry& waiter);

  // Wake up next eligible waiters after a release.
  // SECURITY: FIFO ordering with writer-preference fairness.
  void wakeNextWaiters(LockResourceEntry& entry);

  // Add a waiter to the FIFO queue for a resource.
  WaiterEntry& enqueueWaiter(LockResourceId resource, TransactionId transactionId,
                              LockMode mode, bool isUpgrade);

  // Remove a waiter from the queue (on timeout or cancellation).
  void removeWaiter(LockResourceId resource, TransactionId transactionId);

  LockResourceEntry& getOrCreateResourceEntry(LockResourceId resource) const;

  // Deadlock detection internals (called while mutex_ is held).
  [[nodiscard]] std::vector<TransactionId> DetectDeadlocksInternal(TransactionId startNode) const;

  mutable std::shared_mutex mutex_;  // shared_mutex allows concurrent holds() reads
  std::condition_variable_any condition_;  // shared_mutex compatible
  mutable std::unordered_map<LockResourceId, std::unique_ptr<LockResourceEntry>> locks_;

  std::unique_ptr<DeadlockHandler> deadlockHandler_;
  std::unique_ptr<WaitForGraph> waitGraph_;

  // Diagnostics counters (atomic for lock-free updates)
  std::atomic<std::size_t> totalWaits_{0};
  std::atomic<std::size_t> timeouts_{0};
  std::atomic<std::size_t> deadlocksDetected_{0};
  std::atomic<std::size_t> deadlocksResolved_{0};
  std::atomic<std::size_t> maxWaitQueueLength_{0};

  // Wait time tracking for diagnostics
  struct WaitTimeTracker {
    std::atomic<std::chrono::nanoseconds::rep> totalWaitNs{0};
    std::atomic<std::chrono::nanoseconds::rep> maxWaitNs{0};
    std::atomic<std::size_t> waitCount{0};
  };
  WaitTimeTracker waitTracker_{};
};

class LockGuard {
public:
  LockGuard() = default;
  LockGuard(LockManager& manager, TransactionId transactionId, LockResourceId resource) noexcept;
  ~LockGuard();

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

  LockGuard(LockGuard&& other) noexcept;
  LockGuard& operator=(LockGuard&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return manager_ != nullptr; }
  [[nodiscard]] TransactionId transactionId() const noexcept { return transactionId_; }
  [[nodiscard]] LockResourceId resource() const noexcept { return resource_; }

  void release() noexcept;

private:
  LockManager* manager_{nullptr};
  TransactionId transactionId_{};
  LockResourceId resource_{};
};

} // namespace zenthrildb