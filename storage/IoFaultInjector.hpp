#pragma once

#include <cstddef>

namespace zenthrildb {

enum class IoFaultPoint {
  WalWrite,
  WalPartialWrite,
  WalSync,
  PageWrite,
  PageSync,
  HeaderWrite,
  NoSpace,
  SnapshotPublish,
  RecoveryPageRepair,
  CheckpointPreparing,
  CheckpointWalDurable,
  CheckpointPagesDurable,
  CheckpointPartialFrame,
  CheckpointFrameBuffered,
  CheckpointDurable,
  SnapshotDirectory,
  SnapshotDataPartial,
  SnapshotDataCopied,
  SnapshotWalPartial,
  SnapshotWalCopied,
  SnapshotWalDurable,
  SnapshotDataDurable,
  SnapshotManifestWritten,
  SnapshotManifestDurable,
  SnapshotValidating,
  SnapshotRenamed,
  SnapshotParentDurable,
  SnapshotCreateDirectory,
  SnapshotOpenFile,
  SnapshotReadFile,
  SnapshotWriteFile,
  SnapshotCloseFile,
  SnapshotSyncFile,
  SnapshotSyncDirectory,
  SnapshotRename,
  SnapshotSyncParent,
};

class IoFaultInjector final {
public:
  static void arm(IoFaultPoint point, std::size_t occurrence = 1);
  static void armCrash(IoFaultPoint point);
  static void clear() noexcept;
  [[nodiscard]] static bool isArmed(IoFaultPoint point);
  [[nodiscard]] static bool consume(IoFaultPoint point);
  static void failIfArmed(IoFaultPoint point);
};

} // namespace zenthrildb
