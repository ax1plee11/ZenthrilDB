#include "wal/LogManager.hpp"
#include "wal/LogRecord.hpp"
#include "wal/WriteAheadLog.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
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
