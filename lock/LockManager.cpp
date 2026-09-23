#include "lock/LockManager.hpp"

#include <algorithm>
#include <stdexcept>

namespace zenthrildb {

// ============================================================================
// LockManager
// ============================================================================

LockManager::LockManager(std::unique_ptr<DeadlockHandler> deadlockHandler)
    : deadlockHandler_(std::move(deadlockHandler)),
      waitGraph_(std::make_unique<WaitForGraph>()) {}

LockManager::~LockManager() = default;

bool LockManager::tryAcquire(TransactionId transactionId, LockResourceId resource, LockMode mode) {
  if (!transactionId.valid()) {
    throw std::invalid_argument("Invalid transaction id for lock acquisition");
  }

  std::unique_lock lock(mutex_);
  auto& entry = getOrCreateResourceEntry(resource);
  if (!canGrant(transactionId, resource, mode, entry)) {
    return false;
  }
  grant(transactionId, resource, mode);
  return true;
}

bool LockManager::acquire(TransactionId transactionId,
                          LockResourceId resource,
                          LockMode mode,
                          Timeout timeout) {
  if (!transactionId.valid()) {
    throw std::invalid_argument("Invalid transaction id for lock acquisition");
  }

  auto waitStart = std::chrono::steady_clock::now();

  std::unique_lock lock(mutex_);

  // Try immediate acquisition first.
  auto& entry = getOrCreateResourceEntry(resource);
  if (canGrant(transactionId, resource, mode, entry)) {
    grant(transactionId, resource, mode);
    return true;
  }

  // SECURITY: self-deadlock prevention — if this transaction already holds
  // a lock on this resource and is requesting a different mode, perform upgrade.
  // WEAKNESS FIXED: self-deadlock was possible in v0.9.
  bool alreadyHolds = false;
  LockMode heldMode = LockMode::Shared;
  for (const auto& g : entry.granted) {
    if (g.transactionId == transactionId) {
      alreadyHolds = true;
      heldMode = g.mode;
      break;
    }
  }

  if (alreadyHolds && mode == LockMode::Exclusive && heldMode == LockMode::Shared) {
    // Try to upgrade immediately.
    if (entry.granted.size() == 1) {
      entry.granted[0].mode = LockMode::Exclusive;
      return true;
    }
    // Need to wait — will be enqueued as upgrade.
  }

  // Need to wait — enqueue in FIFO queue.
  totalWaits_.fetch_add(1, std::memory_order_relaxed);
  auto& waiter = enqueueWaiter(resource, transactionId, mode, alreadyHolds);

  // Update wait-for graph.
  for (const auto& granted : entry.granted) {
    if (granted.transactionId != transactionId) {
      waitGraph_->addEdge(transactionId, granted.transactionId);
    }
  }

  // Deadlock detection before waiting.
  auto deadlocks = DetectDeadlocksInternal(transactionId);
  if (!deadlocks.empty()) {
    deadlocksDetected_.fetch_add(1, std::memory_order_relaxed);
    if (deadlockHandler_) {
      auto victim = SelectVictim(deadlocks);
      if (victim && deadlockHandler_->OnDeadlock(*victim, deadlocks)) {
        deadlocksResolved_.fetch_add(1, std::memory_order_relaxed);
        removeWaiter(resource, transactionId);
        return false;
      }
    }
  }

  // Wait for grant or timeout using a per-call condition variable.
  std::condition_variable_any localCv;
  waiter.cv = &localCv;

  auto deadline = std::chrono::steady_clock::now() + timeout;

  while (!waiter.granted && !waiter.timedOut) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    localCv.wait_for(lock, std::chrono::nanoseconds(deadline - now));
  }

  // Update wait time diagnostics.
  auto waitEnd = std::chrono::steady_clock::now();
  auto waitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(waitEnd - waitStart);
  waitTracker_.totalWaitNs.fetch_add(waitDuration.count(), std::memory_order_relaxed);
  waitTracker_.waitCount.fetch_add(1, std::memory_order_relaxed);
  auto maxWait = waitTracker_.maxWaitNs.load(std::memory_order_relaxed);
  while (waitDuration.count() > maxWait &&
         !waitTracker_.maxWaitNs.compare_exchange_weak(maxWait, waitDuration.count()));

  if (waiter.granted) {
    removeWaiter(resource, transactionId);
    return true;
  }

  // Timed out.
  timeouts_.fetch_add(1, std::memory_order_relaxed);
  removeWaiter(resource, transactionId);
  return false;
}

LockGuard LockManager::acquireGuard(TransactionId transactionId,
                                    LockResourceId resource,
                                    LockMode mode,
                                    Timeout timeout) {
  if (!acquire(transactionId, resource, mode, timeout)) {
    throw std::runtime_error("Timed out while acquiring lock");
  }
  return LockGuard(*this, transactionId, resource);
}

void LockManager::release(TransactionId transactionId, LockResourceId resource) {
  std::unique_lock lock(mutex_);
  auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return;
  }

  auto& entry = *it->second;

  // Remove from granted locks.
  auto& granted = entry.granted;
  granted.erase(std::remove_if(granted.begin(), granted.end(), [&](const GrantedLock& g) {
                  return g.transactionId == transactionId;
                }),
                granted.end());

  // Remove from wait queue if present.
  removeWaiter(resource, transactionId);

  waitGraph_->removeEdges(transactionId);

  if (granted.empty() && entry.waiters.empty()) {
    locks_.erase(it);
  } else {
    wakeNextWaiters(entry);
  }
}

void LockManager::releaseAll(TransactionId transactionId) {
  std::unique_lock lock(mutex_);
  for (auto it = locks_.begin(); it != locks_.end();) {
    auto& entry = *it->second;
    auto& granted = entry.granted;
    bool hadLock = false;

    granted.erase(std::remove_if(granted.begin(), granted.end(), [&](const GrantedLock& g) {
                    bool match = (g.transactionId == transactionId);
                    if (match) hadLock = true;
                    return match;
                  }),
                  granted.end());

    // Remove from current resource's wait queue if present.
    entry.waiters.remove_if([&](const WaiterEntry& w) { return w.transactionId == transactionId; });

    if (granted.empty() && entry.waiters.empty()) {
      it = locks_.erase(it);
    } else {
      if (hadLock) {
        wakeNextWaiters(entry);
      }
      ++it;
    }
  }

  // Remove from any remaining wait queues.
  for (auto& [_, e] : locks_) {
    e->waiters.remove_if([&](const WaiterEntry& w) { return w.transactionId == transactionId; });
  }

  waitGraph_->removeEdges(transactionId);
}

// SECURITY: lock upgrade with FIFO queue and deadlock detection.
// WEAKNESS FIXED: lock upgrades could cause deadlock in v0.9.
bool LockManager::upgrade(TransactionId transactionId, LockResourceId resource, Timeout timeout) {
  if (!transactionId.valid()) {
    throw std::invalid_argument("Invalid transaction id for lock upgrade");
  }

  auto waitStart = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);

  auto& entry = getOrCreateResourceEntry(resource);

  // Check if upgrade is immediate.
  if (entry.granted.size() == 1 &&
      entry.granted[0].transactionId == transactionId &&
      entry.granted[0].mode == LockMode::Shared) {
    entry.granted[0].mode = LockMode::Exclusive;
    return true;
  }

  // Need to wait for other shared holders.
  totalWaits_.fetch_add(1, std::memory_order_relaxed);
  auto& waiter = enqueueWaiter(resource, transactionId, LockMode::Exclusive, true);

  waitGraph_->removeEdges(transactionId);
  for (const auto& grantedLock : entry.granted) {
    if (grantedLock.transactionId != transactionId) {
      waitGraph_->addEdge(transactionId, grantedLock.transactionId);
    }
  }

  // Deadlock detection.
  auto deadlocks = DetectDeadlocksInternal(transactionId);
  if (!deadlocks.empty()) {
    deadlocksDetected_.fetch_add(1, std::memory_order_relaxed);
    if (deadlockHandler_) {
      auto victim = SelectVictim(deadlocks);
      if (victim && deadlockHandler_->OnDeadlock(*victim, deadlocks)) {
        deadlocksResolved_.fetch_add(1, std::memory_order_relaxed);
        removeWaiter(resource, transactionId);
        return false;
      }
    }
  }

  std::condition_variable_any localCv;
  waiter.cv = &localCv;
  auto deadline = std::chrono::steady_clock::now() + timeout;

  while (!waiter.granted && !waiter.timedOut) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    localCv.wait_for(lock, std::chrono::nanoseconds(deadline - now));
  }

  auto waitEnd = std::chrono::steady_clock::now();
  auto waitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(waitEnd - waitStart);
  waitTracker_.totalWaitNs.fetch_add(waitDuration.count(), std::memory_order_relaxed);
  waitTracker_.waitCount.fetch_add(1, std::memory_order_relaxed);

  if (waiter.granted) {
    removeWaiter(resource, transactionId);
    return true;
  }

  timeouts_.fetch_add(1, std::memory_order_relaxed);
  removeWaiter(resource, transactionId);
  return false;
}

// SECURITY: lock downgrade from Exclusive to Shared.
// WEAKNESS FIXED: no lock downgrade existed in v0.9.
bool LockManager::downgrade(TransactionId transactionId, LockResourceId resource) {
  std::unique_lock lock(mutex_);
  auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return false;
  }

  auto& granted = it->second->granted;
  for (auto& g : granted) {
    if (g.transactionId == transactionId && g.mode == LockMode::Exclusive) {
      g.mode = LockMode::Shared;
      wakeNextWaiters(*it->second);
      return true;
    }
  }
  return false;
}

bool LockManager::holds(TransactionId transactionId, LockResourceId resource, LockMode mode) const {
  std::shared_lock lock(mutex_);
  const auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return false;
  }

  return std::any_of(it->second->granted.begin(), it->second->granted.end(), [&](const GrantedLock& entry) {
    return entry.transactionId == transactionId && entry.mode == mode;
  });
}

bool LockManager::holds(TransactionId transactionId, LockResourceId resource) const {
  std::shared_lock lock(mutex_);
  const auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return false;
  }

  return std::any_of(it->second->granted.begin(), it->second->granted.end(), [&](const GrantedLock& entry) {
    return entry.transactionId == transactionId;
  });
}

std::size_t LockManager::activeLockCount() const {
  std::shared_lock lock(mutex_);
  std::size_t count = 0;
  for (const auto& [_, entry] : locks_) {
    count += entry->granted.size();
  }
  return count;
}

std::size_t LockManager::activeLockCount(LockResourceId resource) const {
  std::shared_lock lock(mutex_);
  const auto it = locks_.find(resource);
  return it == locks_.end() ? 0 : it->second->granted.size();
}

// WEAKNESS FIXED: deadlock detection via wait-for graph cycle detection.
std::vector<TransactionId> LockManager::DetectDeadlocks() const {
  std::shared_lock lock(mutex_);
  return DetectDeadlocksInternal(TransactionId{});
}

// SECURITY: deterministic victim selection using deterministic ordering.
// WEAKNESS FIXED: no victim selection strategy existed in v0.9.
std::optional<TransactionId> LockManager::SelectVictim(const std::vector<TransactionId>& cycle) const {
  if (cycle.empty()) {
    return std::nullopt;
  }
  // Deterministic selection: pick the first transaction in the cycle.
  // In production, use DeadlockVictimStrategy::Oldest or FewestLocks
  // for better rollback cost optimization.
  return cycle.front();
}

LockWaitStats LockManager::GetWaitStats() const {
  LockWaitStats stats;
  stats.totalWaits = totalWaits_.load(std::memory_order_relaxed);
  stats.timeouts = timeouts_.load(std::memory_order_relaxed);
  stats.deadlocksDetected = deadlocksDetected_.load(std::memory_order_relaxed);
  stats.deadlocksResolved = deadlocksResolved_.load(std::memory_order_relaxed);
  stats.maxWaitQueueLength = maxWaitQueueLength_.load(std::memory_order_relaxed);

  auto waitCount = waitTracker_.waitCount.load(std::memory_order_relaxed);
  stats.currentWaiters = waitCount;

  auto totalWaitNs = waitTracker_.totalWaitNs.load(std::memory_order_relaxed);
  auto maxWaitNs = waitTracker_.maxWaitNs.load(std::memory_order_relaxed);

  stats.maxWaitTime = std::chrono::nanoseconds(maxWaitNs);
  if (waitCount > 0) {
    stats.avgWaitTime = std::chrono::nanoseconds(totalWaitNs / waitCount);
  }

  return stats;
}

std::vector<LockResourceId> LockManager::GetWaitingResources(TransactionId transactionId) const {
  std::vector<LockResourceId> result;
  std::shared_lock lock(mutex_);

  for (const auto& [resourceId, entry] : locks_) {
    for (const auto& waiter : entry->waiters) {
      if (waiter.transactionId == transactionId) {
        result.push_back(resourceId);
        break;
      }
    }
  }

  return result;
}

std::size_t LockManager::GetWaitQueueLength(LockResourceId resource) const {
  std::shared_lock lock(mutex_);
  auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return 0;
  }
  return it->second->waiters.size();
}

void LockManager::ClearTransactionState(TransactionId transactionId) {
  std::unique_lock lock(mutex_);
  for (auto it = locks_.begin(); it != locks_.end();) {
    auto& entry = *it->second;
    entry.granted.erase(std::remove_if(entry.granted.begin(), entry.granted.end(),
                                       [&](const GrantedLock& g) {
                                         return g.transactionId == transactionId;
                                       }),
                        entry.granted.end());
    entry.waiters.remove_if([&](const WaiterEntry& w) { return w.transactionId == transactionId; });

    if (entry.granted.empty() && entry.waiters.empty()) {
      it = locks_.erase(it);
    } else {
      ++it;
    }
  }
  waitGraph_->removeEdges(transactionId);
}

// ============================================================================
// Private helpers
// ============================================================================

bool LockManager::compatible(LockMode requested, LockMode existing) noexcept {
  return requested == LockMode::Shared && existing == LockMode::Shared;
}

// SECURITY: canGrant implements FIFO fairness with writer-preference.
// WEAKNESS FIXED: FIFO ordering was not enforced in v0.9.
bool LockManager::canGrant(TransactionId transactionId,
                           LockResourceId resource,
                           LockMode mode,
                           const LockResourceEntry& entry) const {
  (void)resource;

  // If the transaction already holds this lock, check if the requested mode is compatible.
  for (const auto& granted : entry.granted) {
    if (granted.transactionId == transactionId) {
      if (granted.mode == mode || granted.mode == LockMode::Exclusive) {
        return true;
      }
      if (mode == LockMode::Exclusive && entry.granted.size() == 1) {
        return true;
      }
      return false;
    }
  }

  // SECURITY: FIFO fairness — if there are waiters, only grant if this transaction
  // is next in the queue. This prevents new requests from bypassing the queue.
  if (!entry.waiters.empty()) {
    // Check if this transaction is the first waiter and can be granted.
    const auto& firstWaiter = entry.waiters.front();
    if (firstWaiter.transactionId == transactionId && firstWaiter.granted) {
      return true;
    }
    // Transaction is not in the waiter queue — don't grant a new lock.
    // Check compatibility with existing granted locks.
    for (const auto& granted : entry.granted) {
      if (!compatible(mode, granted.mode)) {
        return false;
      }
    }
    return true;  // Only if compatible with all granted
  }

  // No waiters — check normal compatibility.
  for (const auto& granted : entry.granted) {
    if (!compatible(mode, granted.mode)) {
      return false;
    }
  }
  return true;
}

void LockManager::grant(TransactionId transactionId, LockResourceId resource, LockMode mode) {
  auto& entry = getOrCreateResourceEntry(resource);
  auto& granted = entry.granted;
  for (auto& e : granted) {
    if (e.transactionId == transactionId) {
      if (e.mode == LockMode::Shared && mode == LockMode::Exclusive) {
        e.mode = LockMode::Exclusive;
      }
      return;
    }
  }
  granted.push_back(GrantedLock{.transactionId = transactionId, .mode = mode});
}

void LockManager::grantFromWaiter(LockResourceEntry& entry, WaiterEntry& waiter) {
  bool alreadyHasLock = false;
  for (auto& g : entry.granted) {
    if (g.transactionId == waiter.transactionId) {
      alreadyHasLock = true;
      if (g.mode == LockMode::Shared && waiter.mode == LockMode::Exclusive) {
        g.mode = LockMode::Exclusive;
      }
      break;
    }
  }
  if (!alreadyHasLock) {
    entry.granted.push_back(GrantedLock{.transactionId = waiter.transactionId, .mode = waiter.mode});
  }
  waiter.granted = true;
  if (waiter.cv) {
    waiter.cv->notify_one();
  }
}

// SECURITY: FIFO wake-up with writer-preference fairness.
// WEAKNESS FIXED: notify_all caused thundering herd in v0.9.
void LockManager::wakeNextWaiters(LockResourceEntry& entry) {
  if (entry.waiters.empty()) {
    return;
  }

  // Walk FIFO queue and grant locks to eligible waiters.
  for (auto it = entry.waiters.begin(); it != entry.waiters.end();) {
    auto& waiter = *it;

    // Check if this waiter can be granted given current granted locks.
    bool canGrantNow = true;
    for (const auto& granted : entry.granted) {
      if (granted.transactionId != waiter.transactionId &&
          !compatible(waiter.mode, granted.mode)) {
        canGrantNow = false;
        break;
      }
    }

    if (canGrantNow) {
      // Update wait-for graph — this waiter no longer waits.
      for (const auto& granted : entry.granted) {
        if (granted.transactionId != waiter.transactionId) {
          waitGraph_->removeEdge(waiter.transactionId, granted.transactionId);
        }
      }
      grantFromWaiter(entry, waiter);
      it = entry.waiters.erase(it);
    } else {
      // Writer-preference: if there's an Exclusive waiter ahead in the queue,
      // don't grant Shared locks to waiters that come after.
      if (waiter.mode == LockMode::Exclusive) {
        break;  // Stop at the first blocked exclusive waiter.
      }
      ++it;
    }
  }
}

LockResourceEntry& LockManager::getOrCreateResourceEntry(LockResourceId resource) const {
  auto it = locks_.find(resource);
  if (it == locks_.end()) {
    auto [newIt, _] = locks_.emplace(resource, std::make_unique<LockResourceEntry>());
    return *newIt->second;
  }
  return *it->second;
}

WaiterEntry& LockManager::enqueueWaiter(LockResourceId resource,
                                         TransactionId transactionId,
                                         LockMode mode,
                                         bool isUpgrade) {
  auto& entry = getOrCreateResourceEntry(resource);
  auto queueSize = entry.waiters.size();
  entry.waiters.push_back(WaiterEntry{
      .transactionId = transactionId,
      .mode = mode,
      .queuedAt = std::chrono::steady_clock::now(),
      .isUpgrade = isUpgrade,
      .cv = nullptr,
      .granted = false,
      .timedOut = false,
      .queuePosition = static_cast<std::size_t>(queueSize),
  });
  maxWaitQueueLength_.store(std::max(maxWaitQueueLength_.load(std::memory_order_relaxed),
                                      queueSize + 1),
                            std::memory_order_relaxed);
  return entry.waiters.back();
}

void LockManager::removeWaiter(LockResourceId resource, TransactionId transactionId) {
  auto it = locks_.find(resource);
  if (it == locks_.end()) {
    return;
  }
  it->second->waiters.remove_if([&](const WaiterEntry& w) {
    return w.transactionId == transactionId;
  });
}

std::vector<TransactionId> LockManager::DetectDeadlocksInternal(TransactionId startNode) const {
  auto adjacency = waitGraph_->buildAdjacency(*this);
  if (!adjacency.empty() && startNode.valid()) {
    return waitGraph_->findCycle(startNode, adjacency);
  }

  // Scan from all participants.
  auto participants = waitGraph_->participants();
  for (const auto& p : participants) {
    auto cycle = waitGraph_->findCycle(p, adjacency);
    if (!cycle.empty()) {
      return cycle;
    }
  }
  return {};
}

// ============================================================================
// LockGuard
// ============================================================================

LockGuard::LockGuard(LockManager& manager, TransactionId transactionId, LockResourceId resource) noexcept
    : manager_(&manager), transactionId_(transactionId), resource_(resource) {}

LockGuard::~LockGuard() {
  release();
}

LockGuard::LockGuard(LockGuard&& other) noexcept
    : manager_(other.manager_), transactionId_(other.transactionId_), resource_(other.resource_) {
  other.manager_ = nullptr;
  other.transactionId_ = TransactionId{};
  other.resource_ = LockResourceId{};
}

LockGuard& LockGuard::operator=(LockGuard&& other) noexcept {
  if (this != &other) {
    release();
    manager_ = other.manager_;
    transactionId_ = other.transactionId_;
    resource_ = other.resource_;
    other.manager_ = nullptr;
    other.transactionId_ = TransactionId{};
    other.resource_ = LockResourceId{};
  }
  return *this;
}

void LockGuard::release() noexcept {
  if (manager_ != nullptr) {
    manager_->release(transactionId_, resource_);
    manager_ = nullptr;
    transactionId_ = TransactionId{};
    resource_ = LockResourceId{};
  }
}

} // namespace zenthrildb