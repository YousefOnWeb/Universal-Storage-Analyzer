#include "MftFinder.h"
#include "MftRecord.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>

namespace usa::scanner {

using detail::MftRecord;

namespace {

// --- Volume I/O helpers (Windows only from here on) ---

constexpr std::uint32_t kRootRecordNumber{5};
constexpr std::uint32_t kFirstUserRecord{16};
constexpr std::size_t kMaxVolumeBytes{static_cast<std::size_t>(2) * 1024 * 1024 * 1024};
constexpr std::size_t kReadChunk{4 * 1024 * 1024};
constexpr unsigned kMaxParseThreads{16};

struct RaiiHandle {
    HANDLE h{INVALID_HANDLE_VALUE};
    ~RaiiHandle()
    {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    RaiiHandle(const RaiiHandle&) = delete;
    RaiiHandle& operator=(const RaiiHandle&) = delete;
    RaiiHandle() = default;
    explicit RaiiHandle(HANDLE handle)
        : h(handle)
    {
    }
};

bool readExact(HANDLE h, std::uint64_t offset, unsigned char* out, std::size_t bytes)
{
    // Positioned synchronous reads (the handle is opened without overlapped
    // I/O, so SetFilePointerEx + ReadFile is the correct pairing).
    std::size_t done{0};
    while (done < bytes) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset + done);
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
            return false;
        }
        DWORD got{0};
        const DWORD want = static_cast<DWORD>(
            (std::min)(bytes - done, static_cast<std::size_t>(0x40000000)));
        if (!ReadFile(h, out + done, want, &got, nullptr) || got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

int compareNoCase(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str());
}

} // namespace

struct MftFinder::SharedVolume {
    std::wstring drivePrefix{}; // e.g. L"C:"
    std::vector<MftRecord> records{};
    std::unordered_map<std::uint32_t, std::size_t> byNumber{};
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> children{};
    std::uint64_t bytesPerCluster{4096};
};

namespace {

bool readBootSector(const std::wstring& volumePath, std::uint64_t& bytesPerSector,
                    std::uint64_t& sectorsPerCluster, std::uint64_t& mftStartLcn,
                    std::uint64_t& recordSize)
{
    RaiiHandle h{CreateFileW(volumePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                             nullptr)};
    if (h.h == INVALID_HANDLE_VALUE) {
        return false;
    }
    unsigned char boot[512]{};
    DWORD got{0};
    if (!ReadFile(h.h, boot, sizeof(boot), &got, nullptr) || got != sizeof(boot)) {
        return false;
    }
    if (boot[0] != 0xEB && boot[0] != 0xE9) {
        return false;
    }
    if (std::memcmp(boot + 3, "NTFS    ", 8) != 0) {
        return false;
    }
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        return false;
    }
    std::uint16_t bps{0};
    std::memcpy(&bps, boot + 11, sizeof(bps));
    const auto spc = static_cast<std::uint64_t>(boot[13]);
    std::uint64_t mftLcn{0};
    std::memcpy(&mftLcn, boot + 0x30, sizeof(mftLcn));
    const auto cpr = static_cast<std::int8_t>(boot[0x40]);
    bytesPerSector = bps;
    sectorsPerCluster = spc;
    mftStartLcn = mftLcn;
    if (cpr < 0) {
        recordSize = static_cast<std::uint64_t>(1) << static_cast<unsigned>(-cpr);
    } else {
        recordSize = static_cast<std::uint64_t>(cpr) * spc * bps;
    }
    return bps != 0 && spc != 0 && recordSize >= 512 && recordSize % 512 == 0;
}

struct MftExtent {
    std::uint64_t fileOffset{0}; // byte offset within the $MFT stream
    std::uint64_t length{0};
};

bool getMftExtents(HANDLE mftHandle, std::uint64_t clusterSize, std::vector<MftExtent>& out)
{
    struct Input {
        LARGE_INTEGER StartingVcn{};
    };
    struct Extent {
        LARGE_INTEGER NextVcn{};
        LARGE_INTEGER Lcn{};
    };
    // Big enough for dozens of extents per call.
    constexpr DWORD kOutSize{64 * 1024};
    std::vector<unsigned char> outBuf(kOutSize);
    std::uint64_t vcn{0};
    for (;;) {
        Input in{};
        in.StartingVcn.QuadPart = static_cast<LONGLONG>(vcn);
        DWORD returned{0};
        const BOOL ok = DeviceIoControl(mftHandle, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
                                        outBuf.data(), kOutSize, &returned, nullptr);
        const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            return false;
        }
        if (returned < 16) {
            return false;
        }
        std::uint32_t count{0};
        std::memcpy(&count, outBuf.data(), sizeof(count));
        const auto* extents = reinterpret_cast<const Extent*>(outBuf.data() + 16);
        const std::size_t available = (returned - 16) / sizeof(Extent);
        if (count > available) {
            return false;
        }
        std::uint64_t prevVcn = vcn;
        for (std::uint32_t i{0}; i < count; ++i) {
            const std::uint64_t nextVcn = static_cast<std::uint64_t>(extents[i].NextVcn.QuadPart);
            const std::int64_t lcn = static_cast<std::int64_t>(extents[i].Lcn.QuadPart);
            if (nextVcn > prevVcn && lcn >= 0) {
                MftExtent e{};
                e.fileOffset = prevVcn * clusterSize;
                e.length = (nextVcn - prevVcn) * clusterSize;
                out.push_back(e);
            }
            prevVcn = nextVcn;
        }
        if (!ok) { // ERROR_MORE_DATA handled above; success with full buffer?
            vcn = prevVcn;
            if (err == ERROR_SUCCESS) {
                break;
            }
            continue;
        }
        break;
    }
    return !out.empty();
}

} // namespace

MftFinder::MftFinder() = default;
MftFinder::~MftFinder() = default;

bool isDirectScanAvailable(const std::filesystem::path& root)
{
    // Elevation check.
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    RaiiHandle token{rawToken};
    TOKEN_ELEVATION elevation{};
    DWORD size{0};
    if (!GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &size)
        || elevation.TokenIsElevated == 0) {
        return false;
    }
    // NTFS check on the target volume.
    std::wstring vol = root.root_path().native();
    if (!vol.empty() && vol.back() != L'\\') {
        vol += L'\\';
    }
    wchar_t fsName[MAX_PATH]{};
    if (!GetVolumeInformationW(vol.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName,
                               MAX_PATH)) {
        return false;
    }
    return _wcsicmp(fsName, L"NTFS") == 0;
}

bool MftFinder::loadVolume(const std::filesystem::path& anyPathOnVolume)
{
    lastError_.clear();
    // Captures the failing step plus the Win32 code at the point of failure.
    const auto fail = [&](const char* step) {
        const DWORD code = GetLastError();
        lastError_ = std::string(step) + " (win32=" + std::to_string(code) + ")";
        return false;
    };
    const std::wstring drive = anyPathOnVolume.root_name().native(); // e.g. L"C:"
    if (drive.size() < 2 || drive[1] != L':') {
        return fail("not a drive-letter path");
    }
    // Process-wide volume cache: the engine's probe load warms it and all
    // worker instances share the image instead of re-reading the volume.
    // Volumes are effectively immutable during a scan; a rescan reloads
    // only when the weak reference has expired.
    static std::mutex cacheMutex{};
    static std::unordered_map<std::wstring, std::weak_ptr<SharedVolume>> cache{};
    {
        const std::lock_guard<std::mutex> lock{cacheMutex};
        const auto cached = cache.find(drive);
        if (cached != cache.end()) {
            if (auto shared = cached->second.lock()) {
                volume_ = std::move(shared);
                return true;
            }
        }
    }
    if (volume_ && volume_->drivePrefix == drive) {
        return true; // cached
    }
    const std::wstring volumePath = L"\\\\.\\" + drive;

    std::uint64_t bytesPerSector{0};
    std::uint64_t sectorsPerCluster{0};
    std::uint64_t mftStartLcn{0};
    std::uint64_t recordSize{0};
    if (!readBootSector(volumePath, bytesPerSector, sectorsPerCluster, mftStartLcn, recordSize)) {
        return fail("boot sector read/validation");
    }
    const std::uint64_t clusterSize = bytesPerSector * sectorsPerCluster;
    (void)mftStartLcn; // located via retrieval pointers below, not the LCN hint

    RaiiHandle volume{CreateFileW(volumePath.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr)};
    if (volume.h == INVALID_HANDLE_VALUE) {
        return fail("volume open (raw device)");
    }
    // Open $MFT itself by its well-known file ID (0). Retrieval queries and
    // reads below are stream-relative on this handle.
    FILE_ID_DESCRIPTOR mftId{};
    mftId.dwSize = sizeof(mftId);
    mftId.Type = FileIdType;
    mftId.FileId.QuadPart = 0;
    RaiiHandle mft{OpenFileById(volume.h, &mftId, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN)};
    if (mft.h == INVALID_HANDLE_VALUE) {
        return fail("MFT open by file ID");
    }
    std::vector<MftExtent> extents{};
    if (!getMftExtents(mft.h, clusterSize, extents)) {
        return fail("retrieval pointers (MFT extents)");
    }
    std::uint64_t total{0};
    for (const auto& e : extents) {
        total += e.length;
    }
    if (total == 0 || total > kMaxVolumeBytes || total % recordSize != 0) {
        return fail("implausible MFT size");
    }

    // Unbuffered I/O needs page-aligned memory.
    void* raw = VirtualAlloc(nullptr, static_cast<SIZE_T>(total), MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (raw == nullptr) {
        return fail("image allocation (VirtualAlloc)");
    }
    unsigned char* image = static_cast<unsigned char*>(raw);
    for (const auto& e : extents) {
        std::uint64_t done{0};
        while (done < e.length) {
            const std::uint64_t step = (std::min)(e.length - done, kReadChunk);
            if (!readExact(mft.h, e.fileOffset + done, image + e.fileOffset + done, step)) {
                VirtualFree(raw, 0, MEM_RELEASE);
                return fail("MFT extent read");
            }
            done += step;
        }
    }

    // Parallel record parsing (stride across threads for even load).
    const std::size_t count = static_cast<std::size_t>(total / recordSize);
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 4;
    }
    threads = (std::min)(threads, kMaxParseThreads);
    std::vector<std::vector<MftRecord>> local(threads);
    std::vector<std::thread> workers{};
    for (unsigned t{0}; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (std::size_t i = t; i < count; i += threads) {
                MftRecord rec{};
                if (!detail::parseMftRecord(image + i * recordSize,
                                              static_cast<std::size_t>(recordSize),
                                              static_cast<std::uint32_t>(i), rec)) {
                    continue;
                }
                if (rec.number >= kFirstUserRecord || rec.number == kRootRecordNumber) {
                    local[t].push_back(std::move(rec));
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    VirtualFree(raw, 0, MEM_RELEASE);

    auto shared = std::make_shared<SharedVolume>();
    shared->drivePrefix = drive;
    shared->bytesPerCluster = clusterSize;
    for (auto& vec : local) {
        for (auto& rec : vec) {
            if (rec.physicalIsClusterCount) {
                rec.physicalSize *= clusterSize;
                rec.physicalIsClusterCount = false;
            }
            const std::size_t idx = shared->records.size();
            shared->byNumber.emplace(rec.number, idx);
            shared->records.push_back(std::move(rec));
        }
    }
    if (shared->records.empty()) {
        return fail("no usable records parsed");
    }
    for (std::size_t i{0}; i < shared->records.size(); ++i) {
        const auto& rec = shared->records[i];
        if (rec.number == kRootRecordNumber) {
            continue;
        }
        shared->children[rec.parentNumber].push_back(rec.number);
    }
    // The volume root must resolve or the image is unusable.
    if (shared->byNumber.find(kRootRecordNumber) == shared->byNumber.end()) {
        return fail("volume root record missing");
    }
    {
        const std::lock_guard<std::mutex> lock{cacheMutex};
        cache[drive] = shared;
    }
    volume_ = std::move(shared);
    return true;
}

std::size_t MftFinder::recordCount() const
{
    return volume_ ? volume_->records.size() : 0;
}

bool MftFinder::volumeLoaded() const
{
    return volume_ != nullptr;
}

bool MftFinder::open(const std::filesystem::path& dir)
{
    close();
    if (!loadVolume(dir)) {
        return false;
    }
    // Resolve the directory by descending from the volume root.
    std::uint32_t current = kRootRecordNumber;
    std::error_code ec{};
    const std::filesystem::path rel = std::filesystem::relative(dir, dir.root_path(), ec);
    if (!ec) {
        for (const auto& part : rel) {
            const std::wstring want = part.native();
            if (want == L"." || want.empty()) {
                continue;
            }
            bool found{false};
            const auto it = volume_->children.find(current);
            if (it != volume_->children.end()) {
                for (const std::uint32_t childNum : it->second) {
                    const auto recIt = volume_->byNumber.find(childNum);
                    if (recIt == volume_->byNumber.end()) {
                        continue;
                    }
                    const MftRecord& rec = volume_->records[recIt->second];
                    // Sequence validation is best-effort here: stale parent
                    // references stay listable but are never descended into
                    // as anything but a leaf listing.
                    if (compareNoCase(rec.name, want) == 0) {
                        current = childNum;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    const auto recIt = volume_->byNumber.find(current);
    if (recIt == volume_->byNumber.end()) {
        return false;
    }
    if (!volume_->records[recIt->second].isDirectory) {
        return false;
    }
    openedRecord_ = current;
    openedPath_ = dir;
    const auto childIt = volume_->children.find(current);
    children_ = (childIt != volume_->children.end()) ? childIt->second : std::vector<std::uint32_t>{};
    position_ = 0;
    enumerationStarted_ = false;
    return true;
}

void MftFinder::close()
{
    openedPath_.clear();
    openedRecord_ = 0;
    children_.clear();
    position_ = 0;
    enumerationStarted_ = false;
}

bool MftFinder::findNext()
{
    if (!volume_) {
        return false;
    }
    enumerationStarted_ = true;
    while (position_ < children_.size()) {
        const std::uint32_t num = children_[position_++];
        const auto it = volume_->byNumber.find(num);
        if (it == volume_->byNumber.end()) {
            continue;
        }
        const MftRecord& rec = volume_->records[it->second];
        // Skip metadata entries that leaked past the load filter.
        if (!rec.name.empty() && rec.name[0] == L'$'
            && rec.number < kFirstUserRecord) {
            continue;
        }
        DirEntry cur{};
        cur.fullPath = openedPath_ / rec.name;
        cur.isDirectory = rec.isDirectory && !rec.isReparsePoint;
        cur.isReparsePoint = rec.isReparsePoint;
        cur.logicalSize = rec.isDirectory ? 0 : rec.logicalSize;
        cur.physicalSize = rec.isDirectory ? 0 : rec.physicalSize;
        cur.lastWriteTs = rec.lastWriteTs;
        current_ = std::move(cur);
        return true;
    }
    return false;
}

} // namespace usa::scanner

#else // Non-Windows: direct volume reading is unavailable.

namespace usa::scanner {

MftFinder::MftFinder() = default;
MftFinder::~MftFinder() = default;

bool MftFinder::open(const std::filesystem::path&)
{
    return false;
}

bool MftFinder::findNext()
{
    return false;
}

void MftFinder::close()
{
}

bool MftFinder::loadVolume(const std::filesystem::path&)
{
    return false;
}

std::size_t MftFinder::recordCount() const
{
    return 0;
}

bool MftFinder::volumeLoaded() const
{
    return false;
}

bool isDirectScanAvailable(const std::filesystem::path&)
{
    return false;
}

} // namespace usa::scanner

#endif
