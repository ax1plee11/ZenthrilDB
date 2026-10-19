#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"
#include "wal/LogRecord.hpp"
#include "transaction/TransactionLogAdapter.hpp"
#include "storage/Page.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>

using namespace zenthrildb;

namespace {

std::filesystem::path tempPayloadWalPath() {
  const auto path = std::filesystem::temp_directory_path() / "zenthrildb_payload.wal";
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  return path;
}

template <typename Decode>
void assertDecodeFails(std::vector<Byte> bytes, Decode decode) {
  bool threw = false;
  try {
    (void)decode(bytes);
  } catch (...) {
    threw = true;
  }
  assert(threw);
}

} // namespace

void test_log_payload() {
  {
    const PageAllocationPayload source{.pageId = 42, .pageType = PageType::Table, .ownerTableId = 7};
    const auto bytes = LogPayloadCodec::encodePageAllocation(source);
    const auto restored = LogPayloadCodec::decodePageAllocation(bytes);
    assert(restored.pageId == 42);
    assert(restored.pageType == PageType::Table);
    assert(restored.ownerTableId == 7);
  }

  {
    const PageFreePayload source{.pageId = 43, .ownerTableId = 8};
    const auto restored = LogPayloadCodec::decodePageFree(LogPayloadCodec::encodePageFree(source));
    assert(restored.pageId == 43);
    assert(restored.ownerTableId == 8);
  }

  {
    const PageWrittenPayload source{.pageId = 44,
                                    .pageType = PageType::Index,
                                    .pageChecksum = 0x12345678,
                                    .usedBytes = 128,
                                    .pageVersion = 3,
                                    .ownerTableId = 9};
    const auto restored = LogPayloadCodec::decodePageWritten(LogPayloadCodec::encodePageWritten(source));
    assert(restored.pageId == 44);
    assert(restored.pageType == PageType::Index);
    assert(restored.pageChecksum == 0x12345678);
    assert(restored.usedBytes == 128);
    assert(restored.pageVersion == 3);
    assert(restored.ownerTableId == 9);
  }

  {
    Page before;
    before.header().version = Page::kFormatVersionV2;
    before.header().pageId = 0x01020304;
    before.header().pageType = PageType::Table;
    before.header().usedBytes = 3;
    before.payload()[0] = 0x11;
    before.payload()[1] = 0x22;
    before.payload()[2] = 0x33;
    before.setPageLsn(100);

    Page after = before;
    after.header().usedBytes = 4;
    after.payload()[3] = 0x44;
    after.setPageLsn(101);

    const PageImageWrittenPayload source{
      .transactionId = TransactionId{7001},
      .pageId = 0x01020304,
      .previousTxLsn = 88,
      .pageFormatVersion = Page::kFormatVersionV2,
      .beforeImage = before.serialize(),
      .afterImage = after.serialize(),
    };
    const auto bytes = LogPayloadCodec::encodePageImageWritten(source);
    assert(bytes.size() == 16412);
    assert(bytes[0] == 1);
    assert(bytes[4] == 0x59);
    assert(bytes[5] == 0x1B);
    assert(bytes[12] == 0x04);
    assert(bytes[13] == 0x03);
    assert(bytes[14] == 0x02);
    assert(bytes[15] == 0x01);
    assert(bytes[16] == 0x58);
    assert(bytes[24] == Page::kFormatVersionV2);
    assert(bytes[28 + Page::kV2PageLsnOffset] == 100);
    assert(bytes[28 + kDefaultPageSize + Page::kV2PageLsnOffset] == 101);

    const auto restored = LogPayloadCodec::decodePageImageWritten(bytes);
    assert(restored.transactionId == TransactionId{7001});
    assert(restored.pageId == 0x01020304);
    assert(restored.previousTxLsn == 88);
    assert(restored.pageFormatVersion == Page::kFormatVersionV2);
    assert(restored.beforeImage == source.beforeImage);
    assert(restored.afterImage == source.afterImage);
    Page restoredBefore;
    restoredBefore.deserialize(restored.beforeImage);
    Page restoredAfter;
    restoredAfter.deserialize(restored.afterImage);
    assert(restoredBefore.pageLsn() == 100);
    assert(restoredAfter.pageLsn() == 101);

    const LogRecord record{LogSequenceNumber{102}, LogRecordType::PageImageWritten, bytes};
    const auto decodedRecord = LogRecord::deserialize(record.serialize());
    assert(decodedRecord.lsn() == LogSequenceNumber{102});
    assert(decodedRecord.type() == LogRecordType::PageImageWritten);
    assert(decodedRecord.payload() == bytes);
    assert(LogPayloadCodec::decodePageImageWritten(decodedRecord.payload()).afterImage == source.afterImage);

    auto corruptedRecord = record.serialize();
    corruptedRecord.back() ^= 0x01;
    assertDecodeFails(corruptedRecord, [](std::span<const Byte> data) {
      return LogRecord::deserialize(data);
    });

    auto truncated = bytes;
    truncated.pop_back();
    assertDecodeFails(truncated, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto oversized = bytes;
    oversized.push_back(0);
    assertDecodeFails(oversized, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto wrongPayloadVersion = bytes;
    wrongPayloadVersion[0] = 2;
    assertDecodeFails(wrongPayloadVersion, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto wrongPageFormat = bytes;
    wrongPageFormat[24] = 1;
    assertDecodeFails(wrongPageFormat, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto invalidBeforeImage = bytes;
    invalidBeforeImage[28 + PageHeader::serializedSize()] ^= 0x01;
    assertDecodeFails(invalidBeforeImage, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto invalidAfterImage = bytes;
    invalidAfterImage[28 + kDefaultPageSize + PageHeader::serializedSize()] ^= 0x01;
    assertDecodeFails(invalidAfterImage, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    Page mismatchedPage;
    mismatchedPage.header().version = Page::kFormatVersionV2;
    mismatchedPage.header().pageId = 1;
    mismatchedPage.header().pageType = PageType::Table;
    const auto mismatchedImage = mismatchedPage.serialize();
    auto mismatchedPageId = bytes;
    std::copy(mismatchedImage.begin(), mismatchedImage.end(), mismatchedPageId.begin() + 28);
    assertDecodeFails(mismatchedPageId, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto malformedImage = bytes;
    malformedImage[28 + Page::kV2PageLsnOffset] ^= 0x01;
    assertDecodeFails(malformedImage, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageImageWritten(data);
    });

    auto invalidTransaction = source;
    invalidTransaction.transactionId = TransactionId{};
    bool encodeThrew = false;
    try {
      (void)LogPayloadCodec::encodePageImageWritten(invalidTransaction);
    } catch (const std::invalid_argument&) {
      encodeThrew = true;
    }
    assert(encodeThrew);
  }

  {
    const MetadataUpdatedPayload source{.kind = MetadataPayloadKind::PageDirectory,
                                        .pageId = kPageDirectoryPageId,
                                        .metadataVersion = 1,
                                        .payloadChecksum = 99};
    const auto restored = LogPayloadCodec::decodeMetadataUpdated(LogPayloadCodec::encodeMetadataUpdated(source));
    assert(restored.kind == MetadataPayloadKind::PageDirectory);
    assert(restored.pageId == kPageDirectoryPageId);
    assert(restored.metadataVersion == 1);
    assert(restored.payloadChecksum == 99);
  }

  {
    const TransactionPayload source{.transactionId = TransactionId{101}};
    const auto restored = LogPayloadCodec::decodeTransaction(LogPayloadCodec::encodeTransaction(source));
    assert(restored.transactionId == TransactionId{101});
  }

  {
    auto bytes = LogPayloadCodec::encodeTransaction(TransactionPayload{.transactionId = TransactionId{1}});
    bytes[0] = 2;
    assertDecodeFails(bytes, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodeTransaction(data);
    });

    bytes = LogPayloadCodec::encodePageWritten(PageWrittenPayload{.pageId = 1});
    bytes.pop_back();
    assertDecodeFails(bytes, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageWritten(data);
    });

    bytes = LogPayloadCodec::encodePageFree(PageFreePayload{.pageId = 1});
    bytes.push_back(0xEE);
    assertDecodeFails(bytes, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageFree(data);
    });

    bytes = LogPayloadCodec::encodePageAllocation(PageAllocationPayload{.pageId = 1, .pageType = PageType::Table});
    bytes[8] = 0xFF;
    assertDecodeFails(bytes, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodePageAllocation(data);
    });

    bytes = LogPayloadCodec::encodeMetadataUpdated(MetadataUpdatedPayload{.pageId = kDatabaseMetadataPageId});
    bytes[4] = 0xFF;
    assertDecodeFails(bytes, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodeMetadataUpdated(data);
    });

    assertDecodeFails(std::vector<Byte>{}, [](std::span<const Byte> data) {
      return LogPayloadCodec::decodeTransaction(data);
    });
  }

  {
    const auto path = tempPayloadWalPath();
    {
      LogManager log(path);
      TransactionLogAdapter adapter(log);
      (void)adapter.recordBegin(TransactionId{55});
      log.flush();
      const auto records = log.replay();
      assert(records.size() == 1);
      const auto payload = LogPayloadCodec::decodeTransaction(records[0].payload());
      assert(payload.transactionId == TransactionId{55});
    }
    std::filesystem::remove(path);
  }

  {
    const auto bytes = LogPayloadCodec::encodePageAllocation(
      PageAllocationPayload{.pageId = 77, .pageType = PageType::Overflow, .ownerTableId = 17});
    std::vector<std::thread> threads;
    for (int i = 0; i < 12; ++i) {
      threads.emplace_back([&] {
        const auto restored = LogPayloadCodec::decodePageAllocation(bytes);
        assert(restored.pageId == 77);
        assert(restored.pageType == PageType::Overflow);
        assert(restored.ownerTableId == 17);
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
}
