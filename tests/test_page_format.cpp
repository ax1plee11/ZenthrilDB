#include "storage/Page.hpp"

#include "core/CRCManager.hpp"
#include "storage/BinaryIO.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

template <typename Exception, typename Fn>
void expectThrow(Fn&& fn) {
  bool threw = false;
  try {
    fn();
  } catch (const Exception&) {
    threw = true;
  }
  assert(threw);
}

} // namespace

using namespace zenthrildb;

void test_page_format() {
  Page page;
  page.header().pageId = 7;
  page.header().usedBytes = 12;
  const auto bytes = page.serialize();
  Page restored;
  restored.deserialize(bytes);
  assert(restored.header().pageId == 7);
  assert(restored.header().usedBytes == 12);
  assert(bytes.size() == 8192);

  for (std::size_t i = Page::kV1PayloadSize - 8; i < Page::kV1PayloadSize; ++i) {
    page.payload()[i] = static_cast<Byte>(0xA0 + i);
  }
  const auto v1Bytes = page.serialize();
  Page restoredV1;
  restoredV1.deserialize(v1Bytes);
  assert(restoredV1.header().version == Page::kFormatVersionV1);
  assert(std::equal(page.payload().end() - 8, page.payload().end(), restoredV1.payload().end() - 8));
  expectThrow<std::logic_error>([&] { (void)restoredV1.pageLsn(); });

  page.header().usedBytes = 0xFFFFFFFFu;
  Page restoredV1WithLegacyUsedBytes;
  restoredV1WithLegacyUsedBytes.deserialize(page.serialize());
  assert(restoredV1WithLegacyUsedBytes.header().usedBytes == 0xFFFFFFFFu);

  Page v2;
  v2.header().version = Page::kFormatVersionV2;
  v2.header().pageId = 19;
  v2.header().pageType = PageType::Index;
  v2.header().usedBytes = 5;
  v2.payload()[0] = 0x11;
  v2.payload()[4] = 0x55;
  v2.setPageLsn(0x0102030405060708ull);
  const auto v2Bytes = v2.serialize();
  assert(v2Bytes.size() == 8192);
  assert(v2Bytes[8184] == 0x08);
  assert(v2Bytes[8185] == 0x07);

  Page restoredV2;
  restoredV2.deserialize(v2Bytes);
  assert(restoredV2.header().version == Page::kFormatVersionV2);
  assert(restoredV2.header().pageId == 19);
  assert(restoredV2.header().pageType == PageType::Index);
  assert(restoredV2.header().usedBytes == 5);
  assert(restoredV2.payload().size() == 8164);
  assert(restoredV2.payload()[0] == 0x11);
  assert(restoredV2.payload()[4] == 0x55);
  assert(restoredV2.pageLsn() == 0x0102030405060708ull);

  auto corruptData = v2Bytes;
  corruptData[PageHeader::serializedSize()] ^= 0x01;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(corruptData); });

  auto corruptLsn = v2Bytes;
  corruptLsn[Page::kV2PageLsnOffset + 3] ^= 0x80;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(corruptLsn); });

  auto corruptPageId = v2Bytes;
  corruptPageId[0] ^= 0x01;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(corruptPageId); });

  auto corruptPageType = v2Bytes;
  corruptPageType[4] ^= 0x01;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(corruptPageType); });

  auto corruptUsedBytes = v2Bytes;
  corruptUsedBytes[12] ^= 0x01;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(corruptUsedBytes); });

  auto oversizedUsedBytes = v2Bytes;
  std::size_t usedBytesOffset = 12;
  binary::writeLittleEndian<std::uint32_t>(oversizedUsedBytes, usedBytesOffset, 8165);
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(oversizedUsedBytes); });

  Page fullV2;
  fullV2.header().version = Page::kFormatVersionV2;
  fullV2.header().pageId = 20;
  fullV2.header().usedBytes = static_cast<std::uint32_t>(Page::kV2PayloadSize);
  fullV2.payload().back() = 0xFE;
  Page fullV2Restored;
  fullV2Restored.deserialize(fullV2.serialize());
  assert(fullV2Restored.header().usedBytes == 8164);
  assert(fullV2Restored.payload().back() == 0xFE);

  auto checksumInput = v2Bytes;
  std::size_t checksumOffset = 8;
  const auto storedChecksum = binary::readLittleEndian<std::uint32_t>(checksumInput, checksumOffset);
  std::fill_n(checksumInput.begin() + 8, sizeof(std::uint32_t), Byte{0});
  assert(CRCManager::compute(checksumInput) == storedChecksum);

  fullV2.header().usedBytes = static_cast<std::uint32_t>(Page::kV2PayloadSize + 1);
  expectThrow<std::invalid_argument>([&] { (void)fullV2.serialize(); });

  expectThrow<std::invalid_argument>([&] {
    restoredV2.deserialize(std::span<const Byte>(v2Bytes.data(), v2Bytes.size() - 1));
  });
  Page unsupported;
  unsupported.header().version = 3;
  expectThrow<std::invalid_argument>([&] { (void)unsupported.serialize(); });

  auto unsupportedBytes = v2Bytes;
  unsupportedBytes[16] = 3;
  expectThrow<std::invalid_argument>([&] { restoredV2.deserialize(unsupportedBytes); });
}
