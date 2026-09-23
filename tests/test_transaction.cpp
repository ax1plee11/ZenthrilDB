#include "recovery/RecoveryEngine.hpp"
#include "transaction/TransactionManager.hpp"
#include "wal/LogPayload.hpp"
#include "wal/LogManager.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace zenthrildb;

namespace {

std::filesystem::path tempTransactionWalPath(const char* name) {
  const auto path = std::filesystem::temp_directory_path() / name;
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
  return path;
}

} // namespace

void test_transaction() {
  {
    const auto path = tempTransactionWalPath("zenthrildb_transaction_lifecycle.wal");
    {
      LogManager log(path);
      TransactionManager transactions(log);

      const auto first = transactions.begin();
      assert(first.id().valid());
      assert(transactions.isActive(first.id()));
      assert(transactions.activeCount() == 1);

      transactions.commit(first.id());
      const auto committed = transactions.getTransaction(first.id());
      assert(committed.has_value());
      assert(committed->state() == TransactionState::Committed);
      assert(committed->finishedAtUnixNs() >= committed->startedAtUnixNs());
      assert(transactions.activeCount() == 0);

      bool threw = false;
      try {
        transactions.commit(first.id());
      } catch (...) {
        threw = true;
      }
      assert(threw);

      const auto second = transactions.begin();
      transactions.rollback(second.id());
      const auto rolledBack = transactions.getTransaction(second.id());
      assert(rolledBack.has_value());
      assert(rolledBack->state() == TransactionState::RolledBack);

      log.flush();
      const auto records = log.replay();
      assert(records.size() == 4);
      assert(records[0].type() == LogRecordType::TransactionStarted);
      assert(records[1].type() == LogRecordType::TransactionCommitted);
      assert(records[2].type() == LogRecordType::TransactionStarted);
      assert(records[3].type() == LogRecordType::TransactionRolledBack);
      assert(LogPayloadCodec::decodeTransaction(records[0].payload()).transactionId == first.id());
      assert(LogPayloadCodec::decodeTransaction(records[2].payload()).transactionId == second.id());

      RecoveryEngine engine;
      const auto plan = engine.analyze(log.prepareRecovery());
      assert(plan.actions().size() == 4);
      assert(std::all_of(plan.actions().begin(), plan.actions().end(), [](const RecoveryAction& action) {
        return action.actionType == RecoveryActionType::TransactionBoundary;
      }));
    }
    std::filesystem::remove(path);
  }

  {
    const auto path = tempTransactionWalPath("zenthrildb_transaction_concurrent.wal");
    {
      LogManager log(path);
      TransactionManager transactions(log);
      std::mutex guard;
      std::vector<std::uint64_t> ids;
      std::vector<std::thread> threads;

      for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
          const auto transaction = transactions.begin();
          transactions.commit(transaction.id());
          std::lock_guard lock(guard);
          ids.push_back(transaction.id().value());
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      std::sort(ids.begin(), ids.end());
      assert(ids.size() == 16);
      for (std::uint64_t expected = 1; expected <= ids.size(); ++expected) {
        assert(ids[expected - 1] == expected);
      }
      assert(transactions.activeCount() == 0);
      assert(log.replay().size() == 32);
    }
    std::filesystem::remove(path);
  }

  {
    const auto path = tempTransactionWalPath("zenthrildb_transaction_finish_race.wal");
    {
      LogManager log(path);
      TransactionManager transactions(log);
      const auto transaction = transactions.begin();
      std::mutex guard;
      std::vector<bool> successes;
      std::vector<std::thread> threads;

      for (int i = 0; i < 12; ++i) {
        threads.emplace_back([&, i] {
          try {
            if (i % 2 == 0) {
              transactions.commit(transaction.id());
            } else {
              transactions.rollback(transaction.id());
            }
            std::lock_guard lock(guard);
            successes.push_back(true);
          } catch (const std::runtime_error&) {
            std::lock_guard lock(guard);
            successes.push_back(false);
          }
        });
      }

      for (auto& thread : threads) {
        thread.join();
      }

      assert(std::count(successes.begin(), successes.end(), true) == 1);
      assert(std::count(successes.begin(), successes.end(), false) == 11);

      const auto finalized = transactions.getTransaction(transaction.id());
      assert(finalized.has_value());
      assert(finalized->state() == TransactionState::Committed ||
             finalized->state() == TransactionState::RolledBack);

      const auto records = log.replay();
      assert(records.size() == 2);
      assert(records[0].type() == LogRecordType::TransactionStarted);
      assert(records[1].type() == LogRecordType::TransactionCommitted ||
             records[1].type() == LogRecordType::TransactionRolledBack);
    }
    std::filesystem::remove(path);
  }
}
