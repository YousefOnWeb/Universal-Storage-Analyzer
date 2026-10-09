#include "MftRecord.h"

#include <cstring>
#include <vector>

namespace usa::scanner::detail {
namespace {

constexpr std::uint32_t kFileAttributeDirectory{0x10};
constexpr std::uint32_t kFileAttributeReparsePoint{0x400};

std::uint16_t readU16(const unsigned char* p)
{
    std::uint16_t v{0};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint32_t readU32(const unsigned char* p)
{
    std::uint32_t v{0};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint64_t readU64(const unsigned char* p)
{
    std::uint64_t v{0};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

std::uint64_t filetimeToUnix(std::uint64_t filetime)
{
    constexpr std::uint64_t kEpochDiff{116444736000000000ULL};
    constexpr std::uint64_t kTicksPerSec{10000000ULL};
    return filetime > kEpochDiff ? (filetime - kEpochDiff) / kTicksPerSec : 0;
}

bool parseDataRuns(const unsigned char* runs, std::size_t maxLen, std::vector<DataRun>& out)
{
    if (runs == nullptr) {
        return false;
    }
    std::size_t pos{0};
    std::int64_t curLcn{0};
    while (pos < maxLen) {
        const unsigned char header = runs[pos++];
        const unsigned lenBytes = header & 0xF;
        const unsigned lcnBytes = (header >> 4) & 0xF;
        if (lenBytes == 0 && lcnBytes == 0) {
            break; // terminator
        }
        if (lenBytes == 0 || lenBytes > 8 || lcnBytes > 8 || pos + lenBytes + lcnBytes > maxLen) {
            break; // malformed tail; keep runs decoded so far
        }
        std::uint64_t runLen{0};
        for (unsigned i{0}; i < lenBytes; ++i) {
            runLen |= static_cast<std::uint64_t>(runs[pos + i]) << (i * 8);
        }
        pos += lenBytes;
        if (lcnBytes == 0) {
            // Sparse run: occupies VCNs but no clusters on disk. The
            // LCN field is absent, so there is nothing more to consume.
            out.push_back(DataRun{runLen, 0, true});
            continue;
        }
        std::int64_t delta{0};
        for (unsigned i{0}; i < lcnBytes; ++i) {
            delta |= static_cast<std::int64_t>(runs[pos + i]) << (i * 8);
        }
        if ((runs[pos + lcnBytes - 1] & 0x80) != 0) {
            delta |= -(static_cast<std::int64_t>(1) << (lcnBytes * 8));
        }
        pos += lcnBytes;
        if (delta == -1) {
            out.push_back(DataRun{runLen, 0, true});
            continue; // sparse run: no storage, LCN chain untouched
        }
        curLcn += delta;
        out.push_back(DataRun{runLen, curLcn, false});
    }
    return true;
}

bool parseMftRecord(const unsigned char* base, std::size_t recordSize, std::uint32_t number,
                    MftRecord& rec)
{
    if (base == nullptr || recordSize < 512 || recordSize % 512 != 0) {
        return false;
    }
    if (std::memcmp(base, "FILE", 4) != 0) {
        return false;
    }
    const std::uint16_t usaOffset = readU16(base + 4);
    const std::uint16_t usaCount = readU16(base + 6);
    const std::uint16_t flags = readU16(base + 22);
    const std::uint16_t attrOffset = readU16(base + 20);
    rec.sequence = readU16(base + 16);

    // Fixup validation: every 512-byte sector tail must carry the USN.
    const std::size_t sectors = recordSize / 512;
    if (usaCount != sectors + 1) {
        return false;
    }
    if (static_cast<std::size_t>(usaOffset) + static_cast<std::size_t>(usaCount) * 2 > recordSize) {
        return false;
    }
    const std::uint16_t usn = readU16(base + usaOffset);
    // Validate on a scratch copy; shared image buffers are never mutated.
    std::vector<unsigned char> scratch(base, base + recordSize);
    unsigned char* p = scratch.data();
    for (std::size_t s{0}; s < sectors; ++s) {
        unsigned char* tail = p + (s + 1) * 512 - 2;
        if (readU16(tail) != usn) {
            return false; // torn or corrupt record
        }
        std::memcpy(tail, p + usaOffset + (s + 1) * 2, 2);
    }

    rec.number = number;
    rec.inUse = (flags & 0x1) != 0;
    rec.isDirectory = (flags & 0x2) != 0;
    if (!rec.inUse) {
        return false;
    }

    bool haveName{false};
    bool hasAttributeList{false};
    bool hasAnyData{false};
    std::uint64_t fallbackAllocSize{0};
    std::size_t off = attrOffset;
    while (off + 8 <= recordSize) {
        const std::uint32_t type = readU32(p + off);
        if (type == 0xFFFFFFFFU) {
            break;
        }
        const std::uint32_t len = readU32(p + off + 4);
        if (len < 8 || off + len > recordSize || off + len <= off) {
            break; // corrupt chain; keep what we have so far
        }
        const bool nonResident = p[off + 8] != 0;
        const std::uint16_t attrFlags = readU16(p + off + 12);

        if (type == 0x20) { // $ATTRIBUTE_LIST
            hasAttributeList = true;
        } else if (type == 0x10 && !nonResident) { // $STANDARD_INFORMATION
            const std::uint32_t contentLen = readU32(p + off + 16);
            const std::uint16_t contentOff = readU16(p + off + 20);
            const std::size_t cstart = off + contentOff;
            if (contentLen >= 36 && cstart + contentLen <= recordSize) {
                rec.lastWriteTs = filetimeToUnix(readU64(p + cstart + 8));
                const std::uint32_t dosAttrs = readU32(p + cstart + 32);
                if ((dosAttrs & kFileAttributeReparsePoint) != 0) {
                    rec.isReparsePoint = true;
                }
                // MS-FSCC 2.1.7 $STANDARD_INFORMATION: owner id at +48.
                if (contentLen >= 52) {
                    rec.ownerId = readU32(p + cstart + 48);
                    rec.hasOwner = true;
                }
            }
        } else if (type == 0x30 && !nonResident && !haveName) { // $FILE_NAME
            const std::uint32_t contentLen = readU32(p + off + 16);
            const std::uint16_t contentOff = readU16(p + off + 20);
            const std::size_t cstart = off + contentOff;
            if (contentLen >= 66 && cstart + contentLen <= recordSize) {
                const std::uint64_t parentRef = readU64(p + cstart);
                const std::uint32_t parentNum =
                    static_cast<std::uint32_t>(parentRef & 0xFFFFFFFFFFFFULL);
                const std::uint16_t parentSeq =
                    static_cast<std::uint16_t>((parentRef >> 48) & 0xFFFFULL);
                const std::uint64_t realSize = readU64(p + cstart + 48);
                // Modification time fallback: normally supplied by
                // $STANDARD_INFORMATION, but nameless-time records exist.
                if (rec.lastWriteTs == 0) {
                    rec.lastWriteTs = filetimeToUnix(readU64(p + cstart + 24));
                }
                const std::uint8_t fnNameLen = p[cstart + 64];
                const std::size_t nameOff = cstart + 66;
                // MS-FSCC 2.4 FILE_NAME: allocated size at +40, real size
                // at +48, attribute flags at +56.
                const std::uint64_t allocSize = readU64(p + cstart + 40);

                if (fallbackAllocSize == 0 || fallbackAllocSize < allocSize) {
                    fallbackAllocSize = allocSize;
                }

                if (fnNameLen > 0
                    && nameOff + static_cast<std::size_t>(fnNameLen) * 2 <= recordSize) {
                    rec.parentNumber = parentNum;
                    rec.parentSequence = parentSeq;
                    rec.logicalSize = realSize;
                    rec.name.assign(reinterpret_cast<const wchar_t*>(p + nameOff), fnNameLen);
                    const std::uint32_t fnFlags = readU32(p + cstart + 56);
                    if ((fnFlags & kFileAttributeReparsePoint) != 0) {
                        rec.isReparsePoint = true;
                    }
                    if ((fnFlags & kFileAttributeDirectory) != 0) {
                        rec.isDirectory = true;
                    }
                    haveName = true;
                }
            }
        } else if (type == 0x80) { // $DATA (both unnamed and named, like WofCompressedData)
            hasAnyData = true;
            // MS-FSCC 2.1.7 $DATA: name length at +9 distinguishes the
            // default (unnamed) stream from alternate streams.
            const bool unnamed = p[off + 9] == 0;
            if (!nonResident) {
                const std::uint32_t residentLen = readU32(p + off + 16); // resident content
                rec.residentBytes += residentLen;
                if (unnamed) {
                    rec.unnamedResidentBytes += residentLen;
                    rec.hasUnnamedData = true;
                }
            } else {
                const std::uint16_t runOff = readU16(p + off + 32);
                const std::uint64_t allocSize = readU64(p + off + 40);
                const std::uint64_t compSize = readU64(p + off + 64);
                if (unnamed) {
                    // MS-FSCC non-resident header: real size at +48.
                    rec.unnamedDataRealSize = readU64(p + off + 48);
                    rec.hasUnnamedData = true;
                }
                if ((attrFlags & 0x1) != 0) {
                    // compressed non-resident stream
                    rec.residentBytes += compSize > 0 ? compSize : allocSize;
                } else {
                    // Decode the data-run list with the shared decoder;
                    // sparse runs occupy no clusters.
                    const std::size_t roff = off + runOff;
                    const std::size_t rend = off + len;
                    std::vector<DataRun> runs{};
                    parseDataRuns(p + roff, rend > roff ? rend - roff : 0, runs);
                    std::uint64_t dataClusters{0};
                    for (const auto& run : runs) {
                        if (!run.sparse) {
                            dataClusters += run.lengthClusters;
                        }
                    }
                    rec.nonResidentClusters += dataClusters;
                }
            }
        }
        off += len;
    }

    if (hasAttributeList && !hasAnyData) {
        // Multi-extent resident edge case: the entire $DATA attribute was pushed to an extension record.
        // We fall back safely to the allocation size from $FILE_NAME to avoid reporting 0 physical bytes.
        rec.physicalSize = fallbackAllocSize;
        rec.usesClusterMath = false;
    } else {
        rec.usesClusterMath = true;
    }

    if (rec.logicalSize == 0 && rec.unnamedResidentBytes > 0) {
        // Stale $FILE_NAME size on freshly written files: the resident
        // content length is exact and current (observed live: realSize 0
        // with 48 resident bytes on a 48-byte file). $FILE_NAME stays
        // authoritative whenever it is nonzero (see fixture: 12345 wins
        // over 12 resident bytes).
        rec.logicalSize = rec.unnamedResidentBytes;
    } else if (rec.logicalSize == 0 && rec.hasUnnamedData && rec.unnamedDataRealSize > 0) {
        rec.logicalSize = rec.unnamedDataRealSize;
    }

    return haveName;
}

} // namespace usa::scanner::detail
