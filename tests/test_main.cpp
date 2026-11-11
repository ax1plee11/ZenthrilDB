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
    std::cerr << "test_crc32" << std::endl;
    test_crc32();
    std::cerr << "test_page_format" << std::endl;
    test_page_format();
    std::cerr << "test_file_manager" << std::endl;
    test_file_manager();
    std::cerr << "test_buffer_manager" << std::endl;
    test_buffer_manager();
    std::cerr << "test_metadata_serialization" << std::endl;
    test_metadata_serialization();
    std::cerr << "test_free_page_manager" << std::endl;
    test_free_page_manager();
    std::cerr << "test_metadata_manager" << std::endl;
    test_metadata_manager();
    std::cerr << "test_page_manager" << std::endl;
    test_page_manager();
    std::cerr << "test_storage_boundaries" << std::endl;
    test_storage_boundaries();
    std::cerr << "test_wal" << std::endl;
    test_wal();
    std::cerr << "test_recovery" << std::endl;
    test_recovery();
    std::cerr << "test_transaction" << std::endl;
    test_transaction();
    std::cerr << "test_log_payload" << std::endl;
    test_log_payload();
    std::cerr << "test_binary_io" << std::endl;
    test_binary_io();
    std::cerr << "test_lock_manager" << std::endl;
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
