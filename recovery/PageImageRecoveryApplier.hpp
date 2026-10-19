#pragma once

#include "core/FileManager.hpp"
#include "recovery/RecoveryExecutor.hpp"

namespace zenthrildb {

// Applies full page images recorded in PageImageWritten WAL records.
class PageImageRecoveryApplier final : public RedoApplier, public UndoApplier {
public:
  explicit PageImageRecoveryApplier(FileManager& fileManager) : fileManager_(fileManager) {}

  void applyRedo(const RecoveryAction& action) override;
  void applyUndo(const RecoveryAction& action) override;

private:
  FileManager& fileManager_;
};

} // namespace zenthrildb
