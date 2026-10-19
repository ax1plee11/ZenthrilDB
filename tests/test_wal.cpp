#include "wal/LogManager.hpp"
#include "wal/LogRecord.hpp"
#include "wal/WriteAheadLog.hpp"
#include "wal/LogPayload.hpp"
#include "storage/Page.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace zenthrildb;

namespace {

std::filesystem::path tempWalPath(const char* name) {
  const auto path = std::filesystem::temp_directory_path() / name;
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  return path;
}

std::vector<Byte> payload(std::initializer_list<Byte> bytes) {
  return std::vector<Byte>(bytes);
}

} // namespace

void test_wal() {
  {
    LogRecord record{LogSequenceNumber{7}, LogRecordType::PageWritten, payload({1, 2, 3})};
    const auto bytes = record.serialize();
    const auto restored = LogRecord::deserialize(std::span<const Byte>(bytes.data(), bytes.size()));
    assert(restored.lsn() == LogSequenceNumber{7});
    assert(restored.type() == LogRecordType::PageWritten);
    assert(restored.payload().size() == 3);

    auto corrupted = bytes;
    corrupted.back() ^= 0xFF;
    bool threw = false;
    try {
      (void)LogRecord::deserialize(std::span<const Byte>(corrupted.data(), corrupted.size()));
    } catch (...) {
      threw = true;
    }
    assert(threw);
  }

  {
    LogRecord record{LogSequenceNumber{0x0102030405060708ull}, LogRecordType::PageWritten, payload({1, 2, 3})};
    const auto bytes = record.serialize();
    assert(bytes[12] == 0x08);
    assert(bytes[13] == 0x07);
    assert(bytes[14] == 0x06);
    assert(bytes[15] == 0x05);
    assert(bytes[28] == 0x03);
    assert(bytes[29] == 0x00);
    assert(bytes[30] == 0x00);
    assert(bytes[31] == 0x00);
  }

  {
    auto bytes = LogRecord{LogSequenceNumber{1}, LogRecordType::PageWritten, payload({})}.serialize();
    bytes[28] = 0xFF;
    bytes[29] = 0xFF;
    bytes[30] = 0xFF;
    bytes[31] = 0x7F;
    bool threw = false;
    try {
      (void)LogRecord::deserialize(std::span<const Byte>(bytes.data(), bytes.size()));
    } catch (...) {
      threw = true;
    }
    assert(threw);
  }

  {
    const auto factoryPath = tempWalPath("zenthrildb_lsn_factory.wal");
    {
      LogManager log(factoryPath);
      const auto first = log.appendWithLsn(LogRecordType::PageImageWritten,
        [](LogSequenceNumber assignedLsn) {
          assert(assignedLsn == LogSequenceNumber{1});
          return payload({static_cast<Byte>(assignedLsn.value()), 0x11});
        });
      const auto second = log.appendWithLsn(LogRecordType::PageImageWritten,
        [](LogSequenceNumber assignedLsn) {
          assert(assignedLsn == LogSequenceNumber{2});
          return payload({static_cast<Byte>(assignedLsn.value()), 0x22, 0x33});
        });
      assert(first == LogSequenceNumber{1});
      assert(second == LogSequenceNumber{2});

      const auto records = log.replay();
      assert(records.size() == 2);
      assert(records[0].lsn() == LogSequenceNumber{1});
      assert(records[1].lsn() == LogSequenceNumber{2});
      assert(records[0].payload()[0] == 1);
      assert(records[1].payload()[0] == 2);
    }
    std::filesystem::remove(factoryPath);
  }

  {
    const auto durablePath = tempWalPath("zenthrildb_lsn_durable.wal");
    {
      LogManager log(durablePath);
      const auto lsn = log.appendWithLsn(LogRecordType::PageImageWritten,
        [](LogSequenceNumber assignedLsn) {
          assert(assignedLsn == LogSequenceNumber{1});
          return payload({0x7A, 0x5E, 0x9C});
        });
      assert(lsn == LogSequenceNumber{1});
    }
    {
      LogManager reopened(durablePath);
      const auto records = reopened.replay();
      assert(records.size() == 1);
      assert(records.front().lsn() == LogSequenceNumber{1});
      assert(records.front().payload()[0] == 0x7A);
      assert(records.front().payload()[1] == 0x5E);
      assert(records.front().payload()[2] == 0x9C);
    }
    std::filesystem::remove(durablePath);
  }

  {
    const auto reopenPath = tempWalPath("zenthrildb_lsn_reopen.wal");
    {
      LogManager log(reopenPath);
      for (std::uint64_t expectedLsn = 1; expectedLsn <= 3; ++expectedLsn) {
        const auto lsn = log.appendWithLsn(LogRecordType::PageImageWritten,
          [expectedLsn](LogSequenceNumber assignedLsn) {
            assert(assignedLsn == LogSequenceNumber{expectedLsn});
            return payload({static_cast<Byte>(assignedLsn.value()), 0xC3});
          });
        assert(lsn == LogSequenceNumber{expectedLsn});
      }
    }
    {
      LogManager reopened(reopenPath);
      const auto replayed = reopened.replay();
      assert(replayed.size() == 3);
      for (std::size_t i = 0; i < replayed.size(); ++i) {
        const auto expectedLsn = static_cast<std::uint64_t>(i + 1);
        assert(replayed[i].lsn() == LogSequenceNumber{expectedLsn});
        assert(replayed[i].payload()[0] == static_cast<Byte>(expectedLsn));
      }

      const auto nextLsn = reopened.appendWithLsn(LogRecordType::PageImageWritten,
        [](LogSequenceNumber assignedLsn) {
          return payload({static_cast<Byte>(assignedLsn.value()), 0xD4});
        });
      assert(nextLsn == LogSequenceNumber{4});
      const auto replayedAfterNext = reopened.replay();
      assert(replayedAfterNext.size() == 4);
      assert(replayedAfterNext.back().lsn() == LogSequenceNumber{4});
      assert(replayedAfterNext.back().payload()[0] == 4);
    }
    std::filesystem::remove(reopenPath);
  }

  {
    const auto pageImagePath = tempWalPath("zenthrildb_lsn_page_image.wal");
    {
      LogManager log(pageImagePath);

      Page before;
      before.header().version = Page::kFormatVersionV2;
      before.header().pageId = 42;
      before.header().pageType = PageType::Table;
      before.header().usedBytes = 1;
      before.payload()[0] = 0xB4;
      before.setPageLsn(7);

      Page after = before;
      after.header().usedBytes = 2;
      after.payload()[1] = 0xD2;

      const auto assignedLsn = log.appendWithLsn(
        LogRecordType::PageImageWritten,
        [&](LogSequenceNumber lsn) {
          after.setPageLsn(lsn.value());
          return LogPayloadCodec::encodePageImageWritten(PageImageWrittenPayload{
            .transactionId = TransactionId{123},
            .pageId = 42,
            .previousTxLsn = 6,
            .pageFormatVersion = Page::kFormatVersionV2,
            .beforeImage = before.serialize(),
            .afterImage = after.serialize(),
          });
        });

      const auto records = log.replay();
      assert(records.size() == 1);
      assert(records.front().lsn() == assignedLsn);
      const auto pageImage = LogPayloadCodec::decodePageImageWritten(records.front().payload());
      assert(pageImage.transactionId == TransactionId{123});
      assert(pageImage.pageId == 42);
      assert(pageImage.previousTxLsn == 6);
      assert(pageImage.pageFormatVersion == Page::kFormatVersionV2);
      Page decodedAfterImage;
      decodedAfterImage.deserialize(pageImage.afterImage);
      assert(decodedAfterImage.pageLsn() == records.front().lsn().value());
      assert(decodedAfterImage.pageLsn() == assignedLsn.value());
    }
    std::filesystem::remove(pageImagePath);
  }

  {
    const auto failurePath = tempWalPath("zenthrildb_lsn_factory_failure.wal");
    {
      LogManager log(failurePath);
      bool threw = false;
      try {
        (void)log.appendWithLsn(LogRecordType::PageImageWritten, [](LogSequenceNumber) -> std::vector<Byte> {
          throw std::runtime_error("payload factory failure");
        });
      } catch (const std::runtime_error&) {
        threw = true;
      }
      assert(threw);
      assert(log.replay().empty());

      const auto afterFailure = log.appendWithLsn(LogRecordType::PageImageWritten,
        [](LogSequenceNumber lsn) { return payload({static_cast<Byte>(lsn.value())}); });
      assert(afterFailure == LogSequenceNumber{2});
      const auto records = log.replay();
      assert(records.size() == 1);
      assert(records.front().lsn() == LogSequenceNumber{2});
    }
    std::filesystem::remove(failurePath);
  }

  {
    constexpr std::size_t appendCount = 8;
    const auto concurrentLsnPath = tempWalPath("zenthrildb_concurrent_lsn_factory.wal");
    {
      LogManager log(concurrentLsnPath);
      std::mutex guard;
      std::vector<LogSequenceNumber> assignedLsns;
      std::vector<std::thread> threads;
      for (std::size_t i = 0; i < appendCount; ++i) {
        threads.emplace_back([&] {
          const auto lsn = log.appendWithLsn(LogRecordType::PageImageWritten,
            [](LogSequenceNumber assigned) {
              return payload({static_cast<Byte>(assigned.value()), 0xA5});
            });
          std::lock_guard lock(guard);
          assignedLsns.push_back(lsn);
        });
      }
      for (auto& thread : threads) {
        thread.join();
      }

      assert(assignedLsns.size() == appendCount);
      const std::unordered_set<std::uint64_t> uniqueLsns([&] {
        std::unordered_set<std::uint64_t> values;
        for (const auto lsn : assignedLsns) {
          values.insert(lsn.value());
        }
        return values;
      }());
      assert(uniqueLsns.size() == appendCount);

      const auto records = log.replay();
      assert(records.size() == appendCount);
      for (std::size_t i = 0; i < records.size(); ++i) {
        assert(records[i].lsn() == LogSequenceNumber{i + 1});
        assert(records[i].payload()[0] == static_cast<Byte>(records[i].lsn().value()));
      }
    }
    std::filesystem::remove(concurrentLsnPath);
  }

  const auto path = tempWalPath("zenthrildb_test.wal");
  {
    LogManager log(path);
    const auto first = log.append(LogRecordType::DatabaseCreated, payload({10}));
    const auto second = log.append(LogRecordType::PageAllocated, payload({20}));
    const auto checkpoint = log.checkpoint();
    log.flush();
    assert(first == LogSequenceNumber{1});
    assert(second == LogSequenceNumber{2});
    assert(checkpoint == LogSequenceNumber{3});

    const auto records = log.replay();
    assert(records.size() == 3);
    assert(records[2].type() == LogRecordType::Checkpoint);

    auto context = log.prepareRecovery();
    assert(context.records().size() == 3);
  }

  {
    LogManager reopened(path);
    const auto next = reopened.append(LogRecordType::DatabaseOpened);
    assert(next == LogSequenceNumber{4});
    reopened.truncate(LogSequenceNumber{2});
    const auto records = reopened.replay();
    assert(records.size() == 2);
    assert(records.front().lsn() == LogSequenceNumber{3});
  }

  {
    const auto largePath = tempWalPath("zenthrildb_large.wal");
    {
      LogManager log(largePath);
      for (int i = 0; i < 512; ++i) {
        const auto value = static_cast<Byte>(i % 251);
        (void)log.append(LogRecordType::MetadataUpdated, payload({value, value}));
      }
      log.flush();
      assert(log.replay().size() == 512);
    }
    std::filesystem::remove(largePath);
  }

  {
    const auto concurrentPath = tempWalPath("zenthrildb_concurrent.wal");
    {
      LogManager log(concurrentPath);
      std::vector<LogSequenceNumber> lsns;
      std::mutex guard;
      std::vector<std::thread> threads;
      for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
          const auto lsn = log.append(LogRecordType::PageWritten, payload({42}));
          std::lock_guard lock(guard);
          lsns.push_back(lsn);
        });
      }
      for (auto& thread : threads) {
        thread.join();
      }
      assert(lsns.size() == 8);
      assert(log.replay().size() == 8);
    }
    std::filesystem::remove(concurrentPath);
  }

  {
    const auto corruptPath = tempWalPath("zenthrildb_corrupt.wal");
    {
      LogManager log(corruptPath);
      (void)log.append(LogRecordType::RecoveryMarker, payload({9, 9, 9}));
      log.flush();
    }
    {
      std::fstream stream(corruptPath, std::ios::binary | std::ios::in | std::ios::out);
      stream.seekp(-1, std::ios::end);
      char byte{};
      stream.read(&byte, 1);
      stream.clear();
      stream.seekp(-1, std::ios::end);
      byte = static_cast<char>(byte ^ 0x7F);
      stream.write(&byte, 1);
    }
    bool threw = false;
    try {
      LogManager log(corruptPath);
      (void)log.replay();
    } catch (...) {
      threw = true;
    }
    assert(threw);
    std::filesystem::remove(corruptPath);
  }

  std::filesystem::remove(path);
}
