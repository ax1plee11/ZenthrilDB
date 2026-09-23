#pragma once

#include "core/Types.hpp"

#include <deque>
#include <optional>
#include <unordered_set>

namespace zenthrildb {

class FreePageList {
public:
  void add(PageId pageId);
  [[nodiscard]] bool contains(PageId pageId) const;
  [[nodiscard]] std::optional<PageId> acquire();
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] auto begin() const noexcept { return freePages_.begin(); }
  [[nodiscard]] auto end() const noexcept { return freePages_.end(); }

private:
  std::deque<PageId> freePages_;
  std::unordered_set<PageId> freePageSet_;
};

} // namespace zenthrildb
