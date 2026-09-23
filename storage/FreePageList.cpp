#include "storage/FreePageList.hpp"

namespace zenthrildb {

void FreePageList::add(PageId pageId) {
  if (freePageSet_.contains(pageId)) {
    return;
  }
  freePages_.push_back(pageId);
  freePageSet_.insert(pageId);
}

bool FreePageList::contains(PageId pageId) const {
  return freePageSet_.contains(pageId);
}

std::optional<PageId> FreePageList::acquire() {
  if (freePages_.empty()) {
    return std::nullopt;
  }
  const auto pageId = freePages_.front();
  freePages_.pop_front();
  freePageSet_.erase(pageId);
  return pageId;
}

bool FreePageList::empty() const noexcept {
  return freePages_.empty();
}

std::size_t FreePageList::size() const noexcept {
  return freePages_.size();
}

} // namespace zenthrildb
