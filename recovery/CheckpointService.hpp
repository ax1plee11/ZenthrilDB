#pragma once

#include "wal/LogManager.hpp"

namespace zenthrildb {

class CheckpointService {
public:
  explicit CheckpointService(LogManager& logManager);
  [[nodiscard]] LogSequenceNumber checkpoint();

private:
  LogManager& logManager_;
};

} // namespace zenthrildb
