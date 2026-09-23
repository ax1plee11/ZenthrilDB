#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"
#include "transaction/TransactionLogAdapter.hpp"

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
