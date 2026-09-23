#include "storage/DatabaseMetadata.hpp"
#include "storage/TableMetadata.hpp"

#include <cassert>
#include <span>

using namespace zenthrildb;

void test_metadata_serialization() {
  DatabaseMetadata metadata;
  metadata.upsertTable(TableMetadata{1, "users", 3});
  metadata.upsertTable(TableMetadata{2, "orders", 4});

  const auto bytes = metadata.serialize();
  DatabaseMetadata restored;
  restored.deserialize(std::span<const Byte>(bytes.data(), bytes.size()));

  assert(restored.tableExists("users"));
  assert(restored.tableExists("orders"));
}
