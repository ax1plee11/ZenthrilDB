#include <exception>
#include <iostream>
#include <string>
#include <string_view>

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
void test_crash_recovery(const char* executablePath);
int runCrashCommitChild(const char* walPath, const char* readyPath);
int runCrashPageChild(const char* databasePath, const char* walPath, const char* readyPath, bool commit);
int runCrashRecoveryChild(const char* databasePath, const char* walPath, const char* readyPath);
int runCrashMetadataChild(const char* databasePath, std::size_t failWriteAt, bool extendDirectory);
void test_log_payload();
void test_binary_io();
void test_lock_manager();
void test_storage_faults();
void test_header_recovery();
void test_database_runtime();
void test_database_wal_identity();
void test_snapshot_generation();
void test_snapshot_crash(const char* executablePath);
void test_physical_page_recovery();
void test_physical_recovery_crash(const char* executablePath);
int runPhysicalRecoveryCrashChild(const char* path);
void test_safe_checkpoint();
void test_checkpoint_crash(const char* executablePath);
int runCheckpointCrashChild(const char* path, const char* phase);
int runSnapshotPublicationChild(const char*, const char*, const char*);
void test_snapshot_foundation();
void test_snapshot_publication(const char*);
int runSnapshotCrashChild(const char* sourcePath, const char* destinationPath);

int main(int argc, char** argv) {
  if (argc == 5 && std::string_view(argv[1]) == "--snapshot-publication-child") {
    return runSnapshotPublicationChild(argv[2], argv[3], argv[4]);
  }
  if (argc == 4 && std::string_view(argv[1]) == "--checkpoint-crash-child") {
    return runCheckpointCrashChild(argv[2], argv[3]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--physical-recovery-crash-child") {
    return runPhysicalRecoveryCrashChild(argv[2]);
  }
  if (argc == 4 && std::string_view(argv[1]) == "--snapshot-crash-child") {
    return runSnapshotCrashChild(argv[2], argv[3]);
  }
  if (argc == 4 && std::string_view(argv[1]) == "--crash-commit-child") {
    return runCrashCommitChild(argv[2], argv[3]);
  }
  if (argc == 6 && std::string_view(argv[1]) == "--crash-page-child") {
    return runCrashPageChild(argv[2], argv[3], argv[4], std::string_view(argv[5]) == "commit");
  }
  if (argc == 5 && std::string_view(argv[1]) == "--crash-recovery-child") {
    return runCrashRecoveryChild(argv[2], argv[3], argv[4]);
  }
  if (argc == 5 && std::string_view(argv[1]) == "--crash-metadata-child") {
    return runCrashMetadataChild(argv[2], static_cast<std::size_t>(std::stoull(argv[3])),
                                 std::string_view(argv[4]) == "extend");
  }
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--snapshot-only") {
      test_snapshot_foundation();
      test_snapshot_publication(argv[0]);
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--checkpoint-only") {
      test_safe_checkpoint();
      test_checkpoint_crash(argv[0]);
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--security-only") {
      test_database_wal_identity();
      test_snapshot_generation();
      test_snapshot_crash(argv[0]);
      test_physical_page_recovery();
      test_physical_recovery_crash(argv[0]);
      return 0;
    }
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
    std::cerr << "test_crash_recovery" << std::endl;
    test_crash_recovery(argv[0]);
    std::cerr << "test_log_payload" << std::endl;
    test_log_payload();
    std::cerr << "test_binary_io" << std::endl;
    test_binary_io();
    std::cerr << "test_lock_manager" << std::endl;
    test_lock_manager();
    std::cerr << "test_storage_faults" << std::endl;
    test_storage_faults();
    std::cerr << "test_header_recovery" << std::endl;
    test_header_recovery();
    std::cerr << "test_database_runtime" << std::endl;
    test_database_runtime();
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
