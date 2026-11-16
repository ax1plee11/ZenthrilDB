#include "core/DatabaseRuntime.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using zenthrildb::Byte;
using zenthrildb::DatabaseRuntime;

enum class Command {
  VerifySnapshot, VerifyBaseline,
  Checkpoint,
  Create, Check, Backup, Snapshot, CreateTable, RenameTable, DropTable, ListTables, Put, Get, Erase, Scan,
};

struct Invocation {
  std::string_view name;
  Command command;
  std::size_t argumentCount; // Includes the database path.
};

constexpr Invocation invocations[] = {
  {"verify-snapshot", Command::VerifySnapshot, 1},
  {"verify-baseline", Command::VerifyBaseline, 1},
  {"checkpoint", Command::Checkpoint, 1},
  {"create", Command::Create, 1},
  {"check", Command::Check, 1},
  {"backup", Command::Backup, 2},
  {"snapshot", Command::Snapshot, 2},
  {"create-table", Command::CreateTable, 2},
  {"rename-table", Command::RenameTable, 3},
  {"drop-table", Command::DropTable, 2},
  {"list-tables", Command::ListTables, 1},
  {"put", Command::Put, 4},
  {"get", Command::Get, 3},
  {"erase", Command::Erase, 3},
  {"scan", Command::Scan, 2},
};

void printUsage(std::ostream& output) {
  output << "ZenthrilDB " ZENTHRILDB_VERSION "\n"
            "Usage: zenthrildb COMMAND DATABASE [ARGUMENTS]\n"
            "  create | check | checkpoint | list-tables DATABASE\n"
            "  backup | snapshot DATABASE NEW_DIRECTORY\n"
            "  verify-snapshot | verify-baseline DATABASE\n"
            "  create-table | drop-table | scan DATABASE TABLE\n"
            "  rename-table DATABASE TABLE NEW_NAME\n"
            "  put DATABASE TABLE KEY HEX_VALUE\n"
            "  get | erase DATABASE TABLE KEY\n"
            "  --help | --version\n"
            "DATABASE must end in .zdb. Use '-' as HEX_VALUE for an empty value.\n";
}

int hexDigit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  throw std::invalid_argument("HEX_VALUE must contain only hexadecimal digits");
}

std::vector<Byte> decodeValue(std::string_view hex) {
  if (hex == "-") return {};
  if (hex.size() % 2 != 0) {
    throw std::invalid_argument("HEX_VALUE must contain an even number of digits");
  }
  std::vector<Byte> value;
  value.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    value.push_back(static_cast<Byte>(16 * hexDigit(hex[i]) + hexDigit(hex[i + 1])));
  }
  return value;
}

template <typename Range>
std::string encodeHex(const Range& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto octet = static_cast<Byte>(byte);
    hex.push_back(digits[octet >> 4]);
    hex.push_back(digits[octet & 0x0f]);
  }
  return hex;
}

std::filesystem::path pathFromUtf8(std::string_view path) {
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

int notFound(std::string_view entity) {
  std::cerr << "zenthrildb: " << entity << " not found\n";
  return 3;
}

int run(std::span<const std::string> args) {
  if (args.size() == 1 && args[0] == "--help") {
    printUsage(std::cout);
    return 0;
  }
  if (args.size() == 1 && args[0] == "--version") {
    std::cout << ZENTHRILDB_VERSION "\n";
    return 0;
  }

  if (args.empty()) {
    printUsage(std::cerr);
    return 2;
  }
  const auto invocation = std::ranges::find(invocations, std::string_view(args[0]), &Invocation::name);
  if (invocation == std::end(invocations) || args.size() != invocation->argumentCount + 1) {
    printUsage(std::cerr);
    return 2;
  }
  const auto path = pathFromUtf8(args[1]);
  if (path.extension() != ".zdb") {
    throw std::invalid_argument("DATABASE must end in .zdb");
  }
  const auto command = invocation->command;
  if (command == Command::VerifySnapshot || command == Command::VerifyBaseline) {
    const auto manifest = DatabaseRuntime::verifySnapshot(path, command == Command::VerifyBaseline);
    std::cout << "{\"snapshot_lsn\":" << manifest.identity.snapshotLsn
              << ",\"transaction_high_water\":" << manifest.transactionHighWater
              << ",\"pages_checked\":" << manifest.pageCount << "}\n";
    return 0;
  }
  if (command == Command::Snapshot) {
    DatabaseRuntime::snapshotTo(path, pathFromUtf8(args[2]));
    return 0;
  }
  const auto value = command == Command::Put ? decodeValue(args[4]) : std::vector<Byte>{};
  DatabaseRuntime db(path, command == Command::Create);

  switch (command) {
    case Command::VerifySnapshot:
    case Command::VerifyBaseline:
      break;
    case Command::Checkpoint: {
      const auto result = db.checkpoint();
      std::cout << "{\"checkpoint_lsn\":" << result.checkpointLsn.value()
                << ",\"durable_wal_lsn\":" << result.durableWalLsn.value()
                << ",\"transaction_high_water\":" << result.transactionHighWater << "}\n";
      return 0;
    }
    case Command::Create:
      return 0;
    case Command::Check: {
      const auto report = db.checkConsistency();
      std::cout << "{\"pages_checked\":" << report.pagesChecked
                << ",\"wal_records_checked\":" << report.walRecordsChecked
                << ",\"transactions_checked\":" << report.transactionsChecked << "}\n";
      return 0;
    }
    case Command::Backup:
      db.backupTo(pathFromUtf8(args[2]));
      return 0;
    case Command::Snapshot:
      break; // Handled before opening the source runtime.
    case Command::CreateTable:
      (void)db.createTable(args[2]);
      return 0;
    case Command::RenameTable:
      return db.renameTable(args[2], args[3]) ? 0 : notFound("table");
    case Command::DropTable:
      return db.dropTable(args[2]) ? 0 : notFound("table");
    case Command::ListTables:
      for (const auto& table : db.listTables()) std::cout << encodeHex(table) << '\n';
      return 0;
    case Command::Put:
      db.records().put(args[2], args[3], value);
      return 0;
    case Command::Get: {
      const auto stored = db.records().get(args[2], args[3]);
      if (!stored) return notFound("key");
      std::cout << encodeHex(*stored) << '\n';
      return 0;
    }
    case Command::Erase:
      return db.records().erase(args[2], args[3]) ? 0 : notFound("key");
    case Command::Scan:
      for (const auto& [key, stored] : db.records().scan(args[2])) {
        std::cout << "{\"key_hex\":\"" << encodeHex(key) << "\",\"value_hex\":\""
                  << encodeHex(stored) << "\"}\n";
      }
      return 0;
  }
  throw std::logic_error("Unknown command");
}

#ifdef _WIN32
std::string toUtf8(const wchar_t* argument) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) throw std::invalid_argument("Invalid Unicode argument");
  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, utf8.data(), size, nullptr, nullptr) == 0) {
    throw std::invalid_argument("Invalid Unicode argument");
  }
  utf8.pop_back();
  return utf8;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
      args.push_back(toUtf8(argv[i]));
#else
      args.emplace_back(argv[i]);
#endif
    }
    const int result = run(args);
    if (result == 0 && !std::cout.flush()) throw std::runtime_error("Failed to write output");
    return result;
  } catch (const std::invalid_argument& error) {
    std::cerr << "zenthrildb: " << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "zenthrildb: " << error.what() << '\n';
    return 1;
  }
}
