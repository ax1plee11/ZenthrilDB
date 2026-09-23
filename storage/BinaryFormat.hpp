#pragma once

#include "core/Types.hpp"
#include "storage/PageDirectory.hpp"
#include "storage/TableMetadata.hpp"

#include <cstddef>
#include <cstdint>

namespace zenthrildb::format {

inline constexpr std::uint32_t kDatabaseMetadataFormatVersion = 1;
inline constexpr std::uint32_t kTableMetadataFormatVersion = 1;
inline constexpr std::uint32_t kPageDirectoryFormatVersion = 1;
inline constexpr std::uint32_t kFreePageListFormatVersion = 1;

inline constexpr std::size_t kDatabaseMetadataVersionOffset = 0;
inline constexpr std::size_t kDatabaseMetadataNextTableIdOffset = kDatabaseMetadataVersionOffset + sizeof(std::uint32_t);
inline constexpr std::size_t kDatabaseMetadataTableCountOffset = kDatabaseMetadataNextTableIdOffset + sizeof(std::uint64_t);
inline constexpr std::size_t kDatabaseMetadataEntriesOffset = kDatabaseMetadataTableCountOffset + sizeof(std::uint32_t);

inline constexpr std::size_t kTableMetadataFormatVersionOffset = 0;
inline constexpr std::size_t kTableMetadataTableIdOffset = kTableMetadataFormatVersionOffset + sizeof(std::uint32_t);
inline constexpr std::size_t kTableMetadataRootPageIdOffset = kTableMetadataTableIdOffset + sizeof(std::uint64_t);
inline constexpr std::size_t kTableMetadataCreatedAtOffset = kTableMetadataRootPageIdOffset + sizeof(PageId);
inline constexpr std::size_t kTableMetadataRecordCountOffset = kTableMetadataCreatedAtOffset + sizeof(std::uint64_t);
inline constexpr std::size_t kTableMetadataPageCountOffset = kTableMetadataRecordCountOffset + sizeof(std::uint64_t);
inline constexpr std::size_t kTableMetadataTableVersionOffset = kTableMetadataPageCountOffset + sizeof(std::uint64_t);
inline constexpr std::size_t kTableMetadataNameSizeOffset = kTableMetadataTableVersionOffset + sizeof(std::uint32_t);
inline constexpr std::size_t kTableMetadataNameOffset = kTableMetadataNameSizeOffset + sizeof(std::uint32_t);

inline constexpr std::size_t kPageDirectoryVersionOffset = 0;
inline constexpr std::size_t kPageDirectoryCountOffset = kPageDirectoryVersionOffset + sizeof(std::uint32_t);
inline constexpr std::size_t kPageDirectoryEntriesOffset = kPageDirectoryCountOffset + sizeof(std::uint32_t);

inline constexpr std::size_t kFreePageListVersionOffset = 0;
inline constexpr std::size_t kFreePageListCountOffset = kFreePageListVersionOffset + sizeof(std::uint32_t);
inline constexpr std::size_t kFreePageListEntriesOffset = kFreePageListCountOffset + sizeof(std::uint32_t);

} // namespace zenthrildb::format
