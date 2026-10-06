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
        const std::uint8_t nameLen = p[off + 9];
        const std::uint16_t attrFlags = readU16(p + off + 12);

        if (type == 0x10 && !nonResident) { // $STANDARD_INFORMATION
            const std::uint32_t contentLen = readU32(p + off + 16);
            const std::uint16_t contentOff = readU16(p + off + 20);
            const std::size_t cstart = off + contentOff;
            if (contentLen >= 36 && cstart + contentLen <= recordSize) {
                rec.lastWriteTs = filetimeToUnix(readU64(p + cstart + 8));
                const std::uint32_t dosAttrs = readU32(p + cstart + 32);
                if ((dosAttrs & kFileAttributeReparsePoint) != 0) {
                    rec.isReparsePoint = true;
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
        } else if (type == 0x80 && nameLen == 0) { // unnamed $DATA
            if (!nonResident) {
                rec.physicalSize = readU32(p + off + 16); // resident content
                rec.physicalIsClusterCount = false;
            } else {
                const std::uint16_t runOff = readU16(p + off + 32);
                const std::uint64_t allocSize = readU64(p + off + 40);
                const std::uint64_t compSize = readU64(p + off + 64);
                if ((attrFlags & 0x1) != 0) {
                    rec.physicalSize = compSize > 0 ? compSize : allocSize;
                    rec.physicalIsClusterCount = false;
                } else {
                    // Walk the data-run list; sparse runs (LCN -1) occupy
                    // no clusters. Only run lengths matter for the
                    // allocated total; LCNs are delta-coded.
                    std::uint64_t dataClusters{0};
                    std::size_t roff = off + runOff;
                    const std::size_t rend = off + len;
                    while (roff < rend) {
                        const unsigned char header = p[roff++];
                        const unsigned lenBytes = header & 0xF;
                        const unsigned lcnBytes = (header >> 4) & 0xF;
                        if (lenBytes == 0 && lcnBytes == 0) {
                            break;
                        }
                        if (lenBytes == 0 || lenBytes > 8 || lcnBytes > 8
                            || roff + lenBytes + lcnBytes > rend) {
                            break;
                        }
                        std::uint64_t runLen{0};
                        for (unsigned i{0}; i < lenBytes; ++i) {
                            runLen |= static_cast<std::uint64_t>(p[roff + i]) << (i * 8);
                        }
                        roff += lenBytes;
                        if (lcnBytes == 0) {
                            continue;
                        }
                        std::int64_t delta{0};
                        for (unsigned i{0}; i < lcnBytes; ++i) {
                            delta |= static_cast<std::int64_t>(p[roff + i]) << (i * 8);
                        }
                        if ((p[roff + lcnBytes - 1] & 0x80) != 0) {
                            delta |= -(static_cast<std::int64_t>(1) << (lcnBytes * 8));
                        }
                        roff += lcnBytes;
                        if (delta == -1) {
                            continue; // sparse run: no storage
                        }
                        dataClusters += runLen;
                    }
                    rec.physicalSize = dataClusters;
                    rec.physicalIsClusterCount = true;
                }
            }
        }
        off += len;
    }
    return haveName;
}

} // namespace usa::scanner::detail
