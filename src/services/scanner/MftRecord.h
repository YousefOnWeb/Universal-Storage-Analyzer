#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Portable NTFS record parsing (pure byte wrangling, no OS calls).
// Shared by the direct volume reader; unit-tested everywhere without
// elevation by feeding hand-crafted record images.
namespace usa::scanner::detail {

struct MftRecord {
    std::uint32_t number{0};
    std::uint16_t sequence{0};
    std::uint32_t parentNumber{0};
    std::uint16_t parentSequence{0};
    std::uint32_t ownerId{0}; // $STANDARD_INFORMATION owner, when present
    bool hasOwner{false};
    bool isDirectory{false};
    bool inUse{false};
    bool isReparsePoint{false};
    // When false, physicalSize is used directly (fallback).
    // When true, MftFinder will calculate physicalSize = residentBytes + (nonResidentClusters * clusterSize).
    bool usesClusterMath{false};
    std::uint64_t residentBytes{0};
    std::uint64_t nonResidentClusters{0};

    std::wstring name{};
    std::uint64_t logicalSize{0};
    std::uint64_t physicalSize{0};
    std::uint64_t lastWriteTs{0}; // Unix seconds
    // Unnamed-default-stream extents for the stale-filename fallback (see
    // parseMftRecord): resident content length and non-resident real size.
    std::uint64_t unnamedResidentBytes{0};
    std::uint64_t unnamedDataRealSize{0};
    bool hasUnnamedData{false};
};

std::uint64_t filetimeToUnix(std::uint64_t filetime);

// One data run of a non-resident attribute: lengthClusters of storage
// starting at startLcn (delta-coded on disk). Sparse runs occupy no
// clusters; startLcn is meaningless for them.
struct DataRun {
    std::uint64_t lengthClusters{0};
    std::int64_t startLcn{0};
    bool sparse{false};
};

// Decode a data-run list. Lenient like the on-disk format requires:
// malformed tails stop the walk, keeping runs decoded so far.
bool parseDataRuns(const unsigned char* runs, std::size_t maxLen, std::vector<DataRun>& out);

// Parse one raw MFT record image. Returns true and fills rec when the
// record is usable (valid fixup, in use, has a file name). Never reads
// outside [base, base + recordSize).
bool parseMftRecord(const unsigned char* base, std::size_t recordSize, std::uint32_t number,
                    MftRecord& rec);

} // namespace usa::scanner::detail
