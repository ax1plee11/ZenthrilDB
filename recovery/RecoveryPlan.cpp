#include "recovery/RecoveryPlan.hpp"

#include <utility>

namespace zenthrildb {

void RecoveryPlan::addAction(RecoveryAction action) {
  actions_.push_back(std::move(action));
}

} // namespace zenthrildb
