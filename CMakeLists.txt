cmake_minimum_required(VERSION 3.20)

project(ZenthrilDB
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZENTHRILDB_BUILD_TESTS "Build ZenthrilDB tests" ON)
option(ZENTHRILDB_STATIC_MSVC_RUNTIME "Link the MSVC runtime statically" ON)
if(MSVC AND ZENTHRILDB_STATIC_MSVC_RUNTIME AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(zenthrildb_core
  core/BufferManager.cpp
  core/CRCManager.cpp
  core/StorageConsistencyChecker.cpp
  core/SnapshotBaseline.cpp
  core/StorageConsistencyChecker.hpp
  core/DatabaseRuntime.cpp
  core/WalReductionInstallation.cpp
  storage/WalInstallation.cpp
  core/DatabaseRuntime.hpp
  core/RecordManager.cpp
  core/RecordManager.hpp
  storage/IoFaultInjector.cpp
  storage/IoFaultInjector.hpp
  core/DatabaseFileLock.hpp
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
  recovery/PageImageRecoveryApplier.cpp
  recovery/RecoveryCoordinator.cpp
  recovery/RecoveryEngine.cpp
  recovery/RecoveryExecutor.cpp
  recovery/RecoveryPlan.cpp
  lock/LockManager.cpp
  lock/WaitForGraph.cpp
  transaction/Transaction.cpp
  transaction/TransactionLogAdapter.cpp
  transaction/TransactionManager.cpp
  transaction/StorageMutationCoordinator.cpp
  transaction/StorageMutationCoordinator.hpp
  storage/DatabaseHeader.cpp
  storage/WalIdentity.cpp
  storage/SnapshotManifest.cpp
  storage/DatabaseMetadata.cpp
  storage/DatabaseWalPath.hpp
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
if(WIN32)
  target_link_libraries(zenthrildb_core PRIVATE bcrypt)
endif()

target_compile_options(zenthrildb_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /FS>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(zenthrildb_example
  main.cpp
)

target_link_libraries(zenthrildb_example PRIVATE zenthrildb_core)

add_executable(zenthrildb tools/zenthrildb_cli.cpp)
target_link_libraries(zenthrildb PRIVATE zenthrildb_core)
target_compile_definitions(zenthrildb PRIVATE ZENTHRILDB_VERSION="${PROJECT_VERSION}")
if(MINGW)
  target_link_options(zenthrildb PRIVATE -municode)
endif()

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
    tests/test_crash_recovery.cpp
    tests/test_log_payload.cpp
    tests/test_binary_io.cpp
    tests/test_lock_manager.cpp
    tests/test_storage_faults.cpp
    tests/test_storage_hardening.cpp
    tests/test_header_recovery.cpp
    tests/test_database_runtime.cpp
    tests/test_database_wal_identity.cpp
    tests/test_snapshot_generation.cpp
    tests/test_snapshot_crash.cpp
    tests/test_physical_page_recovery.cpp
    tests/test_physical_recovery_crash.cpp
    tests/test_safe_checkpoint.cpp
    tests/test_checkpoint_crash.cpp
    tests/test_snapshot_foundation.cpp
    tests/test_snapshot_publication.cpp
    tests/test_wal_reduction.cpp
    tests/test_wal_installation.cpp
    tests/test_database_lifecycle.cpp
    tests/benchmark_lifecycle.cpp
  )

  target_link_libraries(zenthrildb_tests PRIVATE zenthrildb_core)
  target_compile_definitions(zenthrildb_core PRIVATE $<$<CONFIG:Debug>:ZENTHRILDB_ENABLE_FAULT_INJECTION>)
  target_compile_definitions(zenthrildb_tests PRIVATE $<$<CONFIG:Debug>:ZENTHRILDB_ENABLE_FAULT_INJECTION>)
  target_compile_options(zenthrildb_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG /FS>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
  )
  add_test(NAME ZenthrilDBTests COMMAND zenthrildb_tests)
  add_test(NAME ZenthrilDBSecurity COMMAND zenthrildb_tests --security-only)
  set_tests_properties(ZenthrilDBSecurity PROPERTIES LABELS security)
  add_test(NAME ZenthrilDBCheckpoint COMMAND zenthrildb_tests --checkpoint-only)
  set_tests_properties(ZenthrilDBCheckpoint PROPERTIES LABELS checkpoint)
  add_test(NAME ZenthrilDBSnapshot COMMAND zenthrildb_tests --snapshot-only)
  set_tests_properties(ZenthrilDBSnapshot PROPERTIES LABELS snapshot)
  add_test(NAME ZenthrilDBReduction COMMAND zenthrildb_tests --reduction-only)
  set_tests_properties(ZenthrilDBReduction PROPERTIES LABELS reduction TIMEOUT 240)
  add_test(NAME ZenthrilDBInstallation COMMAND zenthrildb_tests --installation-only)
  set_tests_properties(ZenthrilDBInstallation PROPERTIES LABELS installation TIMEOUT 300)
  add_test(NAME ZenthrilDBLifecycle COMMAND zenthrildb_tests --lifecycle-only)
  set_tests_properties(ZenthrilDBLifecycle PROPERTIES LABELS lifecycle TIMEOUT 300)
  add_test(NAME ZenthrilDBHardening COMMAND zenthrildb_tests --hardening-only)
  set_tests_properties(ZenthrilDBHardening PROPERTIES LABELS "security;hardening" TIMEOUT 120)
endif()

include(GNUInstallDirs)
install(TARGETS zenthrildb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LIMITATIONS.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES
  docs/CLI_RU.md
  docs/IPC_INTEGRATION.md
  docs/OPERATIONS_RU.md
  docs/RECORD_FORMAT_RU.md
  docs/RELEASE_ACCEPTANCE_RU.md
  docs/ROADMAP.md
  docs/WAL_SPEC.md
  docs/WAL_IDENTITY_RU.md
  docs/PHYSICAL_RECOVERY_RU.md
  docs/SAFE_CHECKPOINT_RU.md
  docs/CHECKPOINT_AUDIT_RU.md
  docs/SNAPSHOT_AUDIT_RU.md
  docs/SNAPSHOT_FOUNDATION_RU.md
  docs/STATUS_STAGE_5_WAL_REDUCTION_RU.md
  docs/STATUS_STAGE_6_CRASH_SAFE_WAL_INSTALLATION_RU.md
  docs/STATUS_STAGE_7_DATABASE_LIFECYCLE_WRITER_SAFETY_RU.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
)
install(DIRECTORY docs/architecture docs/roadmap docs/security
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
  FILES_MATCHING PATTERN "*.md"
)

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZenthrilDB local database and command-line client")
set(CPACK_GENERATOR ZIP TGZ)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
