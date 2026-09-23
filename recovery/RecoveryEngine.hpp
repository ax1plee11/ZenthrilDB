#pragma once

#include "recovery/RecoveryPlan.hpp"
#include "wal/RecoveryContext.hpp"

namespace zenthrildb {

class RecoveryEngine final : public CrashRecovery {
public:
  [[nodiscard]] RecoveryPlan analyze(const RecoveryContext& context) const;
  void recover(const RecoveryContext& context) override;
  [[nodiscard]] const RecoveryPlan& lastPlan() const noexcept { return lastPlan_; }

private:
  [[nodiscard]] RecoveryActionType classify(LogRecordType type) const noexcept;

  RecoveryPlan lastPlan_{};
};

} // namespace zenthrildb
