#include "recovery/PageImageRecoveryApplier.hpp"

#include "wal/LogPayload.hpp"

#include <stdexcept>

namespace zenthrildb {

namespace {

PageImageWrittenPayload decodePageImageAction(const RecoveryAction& action) {
  if (action.recordType != LogRecordType::PageImageWritten) {
    throw std::invalid_argument("Page image recovery applier received a non-page-image action");
  }
  return LogPayloadCodec::decodePageImageWritten(action.payload);
}

Page deserializePage(const std::array<Byte, kDefaultPageSize>& image) {
  Page page;
  page.deserialize(image);
  return page;
}

DatabaseHeader deserializeHeader(const std::array<Byte, DatabaseHeader::serializedSize()>& image) {
  DatabaseHeader header;
  header.deserialize(image);
  return header;
}

} // namespace

void PageImageRecoveryApplier::applyRedo(const RecoveryAction& action) {
  if (action.recordType == LogRecordType::DatabaseHeaderImageWritten) {
    const auto payload = LogPayloadCodec::decodeDatabaseHeaderImageWritten(action.payload);
    const auto current = fileManager_.header().serialize();
    if (current == payload.afterImage) return;
    const auto after = deserializeHeader(payload.afterImage);
    fileManager_.restoreHeaderImage(payload.afterImage,
      after.totalPages < fileManager_.header().totalPages);
    return;
  }
  const auto payload = decodePageImageAction(action);
  try {
    const auto persisted = fileManager_.readPage(payload.pageId);
    if (persisted.header().version == Page::kFormatVersionV2 && persisted.pageLsn() >= action.lsn.value()) {
      return;
    }
  } catch (const PageNotFoundError&) {
    // A page absent from the data file must be restored from its after-image.
  }
  const auto afterImage = deserializePage(payload.afterImage);
  if (afterImage.pageLsn() != action.lsn.value()) {
    throw std::runtime_error("Corrupted WAL: after-image Page LSN does not match record LSN");
  }
  fileManager_.writePage(afterImage);
}

void PageImageRecoveryApplier::applyUndo(const RecoveryAction& action) {
  if (action.recordType == LogRecordType::DatabaseHeaderImageWritten) {
    const auto payload = LogPayloadCodec::decodeDatabaseHeaderImageWritten(action.payload);
    const auto current = fileManager_.header().serialize();
    if (current == payload.beforeImage) return;
    const auto before = deserializeHeader(payload.beforeImage);
    fileManager_.restoreHeaderImage(payload.beforeImage,
      before.totalPages < fileManager_.header().totalPages);
    return;
  }
  const auto payload = decodePageImageAction(action);
  const auto beforeImage = deserializePage(payload.beforeImage);
  if (beforeImage.pageLsn() >= action.lsn.value()) {
    throw std::runtime_error("Corrupted WAL: before-image Page LSN is not older than record LSN");
  }
  fileManager_.writePage(beforeImage);
}

} // namespace zenthrildb
