#include "lock/LockManager.hpp"

#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace zenthrildb;

void test_lock_manager() {
  using namespace std::chrono_literals;

  {
    LockManager locks;
    const auto resource = LockResourceId::table(10);
    assert(locks.tryAcquire(TransactionId{1}, resource, LockMode::Shared));
    assert(locks.tryAcquire(TransactionId{2}, resource, LockMode::Shared));
    assert(!locks.tryAcquire(TransactionId{3}, resource, LockMode::Exclusive));
    assert(locks.activeLockCount(resource) == 2);
    locks.release(TransactionId{1}, resource);
    locks.release(TransactionId{2}, resource);
    assert(locks.tryAcquire(TransactionId{3}, resource, LockMode::Exclusive));
    assert(locks.holds(TransactionId{3}, resource, LockMode::Exclusive));
  }

  {
    LockManager locks;
    const auto resource = LockResourceId::page(99);
    {
      auto guard = locks.acquireGuard(TransactionId{1}, resource, LockMode::Exclusive, 50ms);
      assert(guard.valid());
      assert(!locks.acquire(TransactionId{2}, resource, LockMode::Shared, 10ms));
    }
    assert(locks.acquire(TransactionId{2}, resource, LockMode::Shared, 50ms));
  }

  {
    LockManager locks;
    const auto resource = LockResourceId::metadata(1);
    assert(locks.tryAcquire(TransactionId{1}, resource, LockMode::Shared));
    assert(locks.tryAcquire(TransactionId{1}, resource, LockMode::Exclusive));
    assert(locks.holds(TransactionId{1}, resource, LockMode::Exclusive));
    locks.releaseAll(TransactionId{1});
    assert(locks.activeLockCount() == 0);
  }

  {
    LockManager locks;
    const auto resource = LockResourceId::table(77);
    std::mutex guard;
    std::vector<bool> results;
    std::vector<std::thread> threads;

    for (int i = 0; i < 16; ++i) {
      threads.emplace_back([&, i] {
        const auto acquired = locks.acquire(TransactionId{static_cast<std::uint64_t>(i + 1)},
                                            resource,
                                            LockMode::Shared,
                                            100ms);
        std::lock_guard lock(guard);
        results.push_back(acquired);
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    assert(results.size() == 16);
    for (const auto acquired : results) {
      assert(acquired);
    }
    assert(locks.activeLockCount(resource) == 16);
  }

  {
    LockManager locks;
    const auto resource = LockResourceId::page(7);
    auto exclusive = locks.acquireGuard(TransactionId{1}, resource, LockMode::Exclusive, 50ms);
    bool acquired = true;
    std::thread waiter([&] {
      acquired = locks.acquire(TransactionId{2}, resource, LockMode::Exclusive, 20ms);
    });
    waiter.join();
    assert(!acquired);
  }
}
