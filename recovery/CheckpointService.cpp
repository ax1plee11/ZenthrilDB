#include "recovery/CheckpointService.hpp"

namespace zenthrildb {

CheckpointService::CheckpointService(LogManager& logManager)
  : logManager_(logManager) {}

LogSequenceNumber CheckpointService::checkpoint() {
  return logManager_.checkpoint();
}

} // namespace zenthrildb
