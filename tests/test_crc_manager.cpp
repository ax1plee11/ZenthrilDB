#include "core/CRCManager.hpp"

#include <array>
#include <cassert>

using namespace zenthrildb;

void test_crc32() {
  const std::array<Byte, 4> data{0x01, 0x02, 0x03, 0x04};
  const auto crc = CRCManager::compute(data);
  assert(CRCManager::verify(data, crc));
}
