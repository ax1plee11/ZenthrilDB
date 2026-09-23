#include "wal/RecoveryContext.hpp"

#include <utility>

namespace zenthrildb {

void RecoveryContext::addRecord(LogRecord record) {
  records_.push_back(std::move(record));
}

} // namespace zenthrildb
