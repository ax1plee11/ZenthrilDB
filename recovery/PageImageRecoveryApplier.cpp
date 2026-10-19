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

} // namespace

void PageImageRecoveryApplier::applyRedo(const RecoveryAction& action) {
  const auto payload = decodePageImageAction(action);
  try {
    const auto persisted = fileManager_.readPage(payload.pageId);
    if (persisted.header().version == Page::kFormatVersionV2 && persisted.pageLsn() >= action.lsn.value()) {
      return;
    }
  } catch (const std::runtime_error&) {
    // A page absent from the data file must be restored from its after-image.
  }
  fileManager_.writePage(deserializePage(payload.afterImage));
}

void PageImageRecoveryApplier::applyUndo(const RecoveryAction& action) {
  const auto payload = decodePageImageAction(action);
  fileManager_.writePage(deserializePage(payload.beforeImage));
}

} // namespace zenthrildb
