#pragma once

#include "lock/LockTypes.hpp"

#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zenthrildb {

// WaitForGraph tracks dependencies between transactions for deadlock detection.
// ARCHITECTURE: Wait-for graph edge (A -> B) means transaction A is waiting
// for a lock held by transaction B. A cycle in this graph indicates deadlock.
//
// SECURITY: The graph is read-optimized with shared_mutex for concurrent diagnostics.
// WEAKNESS FIXED: no wait-for graph existed in v0.9.
class WaitForGraph {
public:
  WaitForGraph() = default;
  ~WaitForGraph() = default;

  // Add an edge: waiter is waiting for holder.
  void addEdge(TransactionId waiter, TransactionId holder);

  // Remove all edges involving the given transaction (on release or abort).
  void removeEdges(TransactionId transactionId);

  // Remove a specific edge.
  void removeEdge(TransactionId waiter, TransactionId holder);

  // Clear all edges.
  void clear();

  // Check if there's an edge from waiter to holder.
  [[nodiscard]] bool hasEdge(TransactionId waiter, TransactionId holder) const;

  // Get all transactions that the given transaction is waiting for.
  [[nodiscard]] std::vector<TransactionId> getWaiters(TransactionId holder) const;

  // Get all transactions that are waiting for the given transaction.
  [[nodiscard]] std::vector<TransactionId> getHolders(TransactionId waiter) const;

  // Build adjacency list from the current lock manager state.
  // This reconstructs the graph from the actual granted/waiter state.
  template <typename LockManagerType>
  [[nodiscard]] std::unordered_map<TransactionId, std::vector<TransactionId>>
  buildAdjacency(const LockManagerType& manager) const {
    std::unordered_map<TransactionId, std::vector<TransactionId>> adjacency;

    // Acquire shared lock on LockManager's mutex via its friend access.
    std::shared_lock lock(manager.mutex_);
    for (const auto& [resourceId, entry] : manager.locks_) {
      (void)resourceId;
      for (const auto& waiter : entry->waiters) {
        for (const auto& granted : entry->granted) {
          if (granted.transactionId != waiter.transactionId) {
            adjacency[waiter.transactionId].push_back(granted.transactionId);
          }
        }
      }
    }
    return adjacency;
  }

  // Find a cycle starting from the given node using DFS.
  // Returns the cycle path if found, empty vector otherwise.
  [[nodiscard]] std::vector<TransactionId> findCycle(
      TransactionId startNode,
      const std::unordered_map<TransactionId, std::vector<TransactionId>>& adjacency) const;

  // Get current edge count for diagnostics.
  [[nodiscard]] std::size_t edgeCount() const;

  // Get all participants in the wait-for graph.
  [[nodiscard]] std::vector<TransactionId> participants() const;

private:
  mutable std::shared_mutex mutex_;
  // Edges: waiter -> set of holders it depends on.
  std::unordered_map<TransactionId, std::unordered_set<TransactionId>> edges_;
};

} // namespace zenthrildb