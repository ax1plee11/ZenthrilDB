#include <exception>
#include <iostream>

namespace {
int failures = 0;
}

void test_crc32();
void test_file_manager();
void test_page_format();
void test_buffer_manager();
void test_metadata_serialization();
void test_free_page_manager();
void test_metadata_manager();
void test_page_manager();
void test_storage_boundaries();
void test_wal();
void test_recovery();
void test_transaction();
void test_log_payload();
void test_binary_io();
void test_lock_manager();

int main() {
  try {
    test_crc32();
    test_page_format();
    test_file_manager();
    test_buffer_manager();
    test_metadata_serialization();
    test_free_page_manager();
    test_metadata_manager();
    test_page_manager();
    test_storage_boundaries();
    test_wal();
    test_recovery();
    test_transaction();
    test_log_payload();
    test_binary_io();
    test_lock_manager();
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All ZenthrilDB tests passed\n";
  return 0;
}
