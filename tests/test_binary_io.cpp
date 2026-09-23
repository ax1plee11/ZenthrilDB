#include "storage/BinaryIO.hpp"

#include <array>
#include <cassert>

using namespace zenthrildb;

void test_binary_io() {
  std::array<Byte, 16> bytes{};
  std::size_t offset = 0;
  binary::writeLittleEndian<std::uint32_t>(bytes, offset, 0x01020304u);
  binary::writeLittleEndian<std::uint64_t>(bytes, offset, 0x0102030405060708ull);

  assert(bytes[0] == 0x04);
  assert(bytes[1] == 0x03);
  assert(bytes[2] == 0x02);
  assert(bytes[3] == 0x01);
  assert(bytes[4] == 0x08);
  assert(bytes[11] == 0x01);

  offset = 0;
  assert(binary::readLittleEndian<std::uint32_t>(bytes, offset) == 0x01020304u);
  assert(binary::readLittleEndian<std::uint64_t>(bytes, offset) == 0x0102030405060708ull);

  bool threw = false;
  try {
    binary::writeLittleEndian<std::uint64_t>(bytes, offset, 1);
  } catch (...) {
    threw = true;
  }
  assert(threw);

  offset = bytes.size() - 1;
  threw = false;
  try {
    (void)binary::readLittleEndian<std::uint32_t>(bytes, offset);
  } catch (...) {
    threw = true;
  }
  assert(threw);
}
