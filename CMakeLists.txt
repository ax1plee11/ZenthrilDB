cmake_minimum_required(VERSION 3.20)

project(ZenthrilDB
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZENTHRILDB_BUILD_TESTS "Build ZenthrilDB tests" ON)

add_library(zenthrildb_core
  core/BufferManager.cpp
  core/CRCManager.cpp
  core/FileManager.cpp
  core/FreePageManager.cpp
  core/MetadataManager.cpp
  core/PageManager.cpp
  wal/LogManager.cpp
  wal/LogPayload.cpp
  wal/LogRecord.cpp
  wal/RecoveryContext.cpp
  wal/WriteAheadLog.cpp
  recovery/CheckpointService.cpp
  recovery/RecoveryEngine.cpp
  recovery/RecoveryExecutor.cpp
  recovery/RecoveryPlan.cpp
  lock/LockManager.cpp
  transaction/Transaction.cpp
  transaction/TransactionLogAdapter.cpp
  transaction/TransactionManager.cpp
  storage/DatabaseHeader.cpp
  storage/DatabaseMetadata.cpp
  storage/PageDirectory.cpp
  storage/Page.cpp
  storage/PageHeader.cpp
  storage/FreePageList.cpp
  storage/TableMetadata.cpp
)

target_include_directories(zenthrildb_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(zenthrildb_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(zenthrildb_example
  main.cpp
)

target_link_libraries(zenthrildb_example PRIVATE zenthrildb_core)

if(ZENTHRILDB_BUILD_TESTS)
  enable_testing()

  add_executable(zenthrildb_tests
    tests/test_main.cpp
    tests/test_crc_manager.cpp
    tests/test_file_manager.cpp
    tests/test_page_format.cpp
    tests/test_buffer_manager.cpp
    tests/test_metadata_serialization.cpp
    tests/test_free_page_manager.cpp
    tests/test_metadata_manager.cpp
    tests/test_page_manager.cpp
    tests/test_storage_boundaries.cpp
    tests/test_wal.cpp
    tests/test_recovery.cpp
    tests/test_transaction.cpp
    tests/test_log_payload.cpp
    tests/test_binary_io.cpp
    tests/test_lock_manager.cpp
  )

  target_link_libraries(zenthrildb_tests PRIVATE zenthrildb_core)
  add_test(NAME ZenthrilDBTests COMMAND zenthrildb_tests)
endif()
