#include "lock/WaitForGraph.hpp"

#include <algorithm>

namespace zenthrildb {

void WaitForGraph::addEdge(TransactionId waiter, TransactionId holder) {
  if (!waiter.valid() || !holder.valid()) {
    return;
  }
  std::unique_lock lock(mutex_);
  edges_[waiter].insert(holder);
}

void WaitForGraph::removeEdges(TransactionId transactionId) {
  std::unique_lock lock(mutex_);
  edges_.erase(transactionId);
  // Remove edges where this transaction is a holder.
  for (auto& [tx, holders] : edges_) {
    holders.erase(transactionId);
  }
}

void WaitForGraph::removeEdge(TransactionId waiter, TransactionId holder) {
  std::unique_lock lock(mutex_);
  auto it = edges_.find(waiter);
  if (it != edges_.end()) {
    it->second.erase(holder);
    if (it->second.empty()) {
      edges_.erase(it);
    }
  }
}

void WaitForGraph::clear() {
  std::unique_lock lock(mutex_);
  edges_.clear();
}

bool WaitForGraph::hasEdge(TransactionId waiter, TransactionId holder) const {
  std::shared_lock lock(mutex_);
  auto it = edges_.find(waiter);
  if (it == edges_.end()) {
    return false;
  }
  return it->second.contains(holder);
}

std::vector<TransactionId> WaitForGraph::getWaiters(TransactionId holder) const {
  std::vector<TransactionId> result;
  std::shared_lock lock(mutex_);
  for (const auto& [waiter, holders] : edges_) {
    if (holders.contains(holder)) {
      result.push_back(waiter);
    }
  }
  return result;
}

std::vector<TransactionId> WaitForGraph::getHolders(TransactionId waiter) const {
  std::shared_lock lock(mutex_);
  auto it = edges_.find(waiter);
  if (it == edges_.end()) {
    return {};
  }
  return std::vector<TransactionId>(it->second.begin(), it->second.end());
}

std::size_t WaitForGraph::edgeCount() const {
  std::shared_lock lock(mutex_);
  std::size_t count = 0;
  for (const auto& [_, holders] : edges_) {
    count += holders.size();
  }
  return count;
}

std::vector<TransactionId> WaitForGraph::participants() const {
  std::shared_lock lock(mutex_);
  std::unordered_set<TransactionId> unique;
  for (const auto& [waiter, holders] : edges_) {
    unique.insert(waiter);
    for (const auto& h : holders) {
      unique.insert(h);
    }
  }
  return std::vector<TransactionId>(unique.begin(), unique.end());
}

std::vector<TransactionId> WaitForGraph::findCycle(
    TransactionId startNode,
    const std::unordered_map<TransactionId, std::vector<TransactionId>>& adjacency) const {
  // DFS with three-color marking for cycle detection.
  // white=unvisited, gray=in-progress, black=done
  enum class Color : std::uint8_t { White, Gray, Black };
  std::unordered_map<TransactionId, Color> color;
  std::vector<TransactionId> path;
  std::vector<TransactionId> cycle;

  std::function<bool(TransactionId)> dfs = [&](TransactionId node) -> bool {
    color[node] = Color::Gray;
    path.push_back(node);

    auto it = adjacency.find(node);
    if (it != adjacency.end()) {
      for (const auto& neighbor : it->second) {
        auto neighborColor = color.find(neighbor);
        if (neighborColor == color.end() || neighborColor->second == Color::White) {
          if (dfs(neighbor)) {
            return true;
          }
        } else if (neighborColor->second == Color::Gray) {
          // Back edge — found a cycle.
          cycle.push_back(neighbor);
          // Include all nodes in the cycle.
          for (auto it2 = path.rbegin(); it2 != path.rend(); ++it2) {
            cycle.push_back(*it2);
            if (*it2 == neighbor) {
              break;
            }
          }
          std::reverse(cycle.begin(), cycle.end());
          return true;
        }
      }
    }

    color[node] = Color::Black;
    path.pop_back();
    return false;
  };

  if (dfs(startNode)) {
    return cycle;
  }
  return {};
}

} // namespace zenthrildb