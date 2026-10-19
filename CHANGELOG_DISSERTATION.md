# Changelog for dissertation verification

## 25.09.2026 — LockManager build and waiter-lifetime correction

Purpose: make the current source revision buildable and avoid invalid access to a
waiter entry after a lock grant. This change does not add a database capability.

- Changed `WaiterEntry::cv` from `std::condition_variable*` to
  `std::condition_variable_any*`, matching the concrete synchronization object
  used in `LockManager.cpp`.
- Added `lock/WaitForGraph.cpp` to the CMake target so the declared deadlock
  graph symbols are linked in CMake builds.
- Prevented `wakeNextWaiters()` from erasing a waiter before its waiting thread
  observes `waiter.granted`; the waiting thread remains responsible for removal.
- Removed recursive acquisition of `LockManager::mutex_` during wait-for graph
  adjacency construction. The callers of `DetectDeadlocksInternal()` already
  hold the manager mutex.

Verification is recorded only after a clean current-source build and test run.
