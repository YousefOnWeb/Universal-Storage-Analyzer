#include "MftFinder.h"
#include "MftRecord.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <memory>
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

// VirtualAlloc/VirtualFree pairing as RAII: every early return below must
// release the image, which manual pairing cannot guarantee.
struct VirtualFreeDeleter {
    void operator()(void* p) const noexcept
    {
        if (p != nullptr) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
};
using VirtualImage = std::unique_ptr<void, VirtualFreeDeleter>;

int compareNoCase(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str());
}

} // namespace

struct MftFinder::SharedVolume {
    std::wstring drivePrefix{}; // e.g. L"C:"
    DWORD volumeSerial{0}; // GetVolumeInformationW serial: remount/reformat
                           // changes it, invalidating the cached image
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
    std::uint64_t diskOffset{0}; // byte offset on the volume
    std::uint64_t length{0};
};

// Enumerates the $MFT's storage extents via the volume handle.
// Replaces the old FSCTL_GET_RETRIEVAL_POINTERS approach with FSCTL_GET_NTFS_FILE_RECORD,
// which is the supported user-mode mechanism to bypass file open restrictions on modern Windows.
bool getMftExtents(HANDLE volume, std::uint64_t clusterSize, std::vector<MftExtent>& out,
                   std::string& error)
{
    // A helper to fetch a specific MFT record by its FRN.
    auto fetchRecord = [&](std::uint64_t frn, std::vector<unsigned char>& recordOut) -> bool {
        NTFS_FILE_RECORD_INPUT_BUFFER in{};
        in.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);
        // MFT records are typically 1KB or 4KB. Add space for the output header.
        std::vector<unsigned char> outBuf(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + 4096);
        DWORD returned{0};
        SetLastError(ERROR_SUCCESS);
        if (!DeviceIoControl(volume, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                             outBuf.data(), static_cast<DWORD>(outBuf.size()), &returned, nullptr)) {
            return false;
        }
        if (returned < sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER)) {
            return false;
        }
        auto* header = reinterpret_cast<PNTFS_FILE_RECORD_OUTPUT_BUFFER>(outBuf.data());
        if (header->FileRecordLength < 512 || header->FileRecordLength > 4096) {
            return false;
        }
        recordOut.assign(header->FileRecordBuffer, header->FileRecordBuffer + header->FileRecordLength);
        return true;
    };

    struct DataExtentBlob {
        std::uint64_t lowestVcn;
        std::vector<unsigned char> runsBytes;
    };
    std::vector<DataExtentBlob> dataBlobs;

    // A helper to parse the $DATA runs from a raw MFT record buffer.
    auto parseRecordForData = [&](const std::vector<unsigned char>& recordBuffer,
                                  std::vector<std::uint64_t>& extensionFrns) -> bool {
        const unsigned char* p = recordBuffer.data();
        const std::size_t recordSize = recordBuffer.size();
        if (recordSize < 512 || std::memcmp(p, "FILE", 4) != 0) {
            return false;
        }
        const std::uint16_t attrOffset = *reinterpret_cast<const std::uint16_t*>(p + 20);
        std::size_t off = attrOffset;
        while (off + 8 <= recordSize) {
            const std::uint32_t type = *reinterpret_cast<const std::uint32_t*>(p + off);
            if (type == 0xFFFFFFFFU) {
                break;
            }
            const std::uint32_t len = *reinterpret_cast<const std::uint32_t*>(p + off + 4);
            if (len < 8 || off + len > recordSize || off + len <= off) {
                break;
            }
            const bool nonResident = p[off + 8] != 0;
            const std::uint8_t nameLen = p[off + 9];

            if (type == 0x20) { // $ATTRIBUTE_LIST
                if (!nonResident) {
                    const std::uint32_t contentLen = *reinterpret_cast<const std::uint32_t*>(p + off + 16);
                    const std::uint16_t contentOff = *reinterpret_cast<const std::uint16_t*>(p + off + 20);
                    std::size_t listOff = off + contentOff;
                    const std::size_t listEnd = listOff + contentLen;
                    while (listOff + 26 <= listEnd && listOff + 26 <= recordSize) {
                        const std::uint32_t entryType = *reinterpret_cast<const std::uint32_t*>(p + listOff);
                        const std::uint16_t entryLen = *reinterpret_cast<const std::uint16_t*>(p + listOff + 4);
                        if (entryLen < 26 || listOff + entryLen > listEnd) {
                            break;
                        }
                        if (entryType == 0x80) { // $DATA attribute piece
                            const std::uint64_t ref = *reinterpret_cast<const std::uint64_t*>(p + listOff + 16);
                            const std::uint64_t extFrn = ref & 0xFFFFFFFFFFFFULL;
                            if (extFrn != 0) { // skip base record if listed
                                extensionFrns.push_back(extFrn);
                            }
                        }
                        listOff += entryLen;
                    }
                }
            } else if (type == 0x80 && nameLen == 0) { // Unnamed $DATA
                if (nonResident) {
                    const std::uint16_t runOff = *reinterpret_cast<const std::uint16_t*>(p + off + 32);
                    const std::size_t roff = off + runOff;
                    const std::size_t rend = off + len;
                    std::uint64_t currentVcn = *reinterpret_cast<const std::uint64_t*>(p + off + 16); // Lowest VCN
                    if (rend > roff) {
                        dataBlobs.push_back({currentVcn, std::vector<unsigned char>(p + roff, p + rend)});
                    }
                }
            }
            off += len;
        }
        return true;
    };

    std::vector<unsigned char> baseRecord;
    if (!fetchRecord(0, baseRecord)) {
        error = "failed to read MFT base record (win32=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    std::vector<std::uint64_t> extensionFrns;
    if (!parseRecordForData(baseRecord, extensionFrns)) {
        error = "failed to parse MFT base record";
        return false;
    }

    std::sort(extensionFrns.begin(), extensionFrns.end());
    extensionFrns.erase(std::unique(extensionFrns.begin(), extensionFrns.end()), extensionFrns.end());

    for (std::uint64_t frn : extensionFrns) {
        std::vector<unsigned char> extRecord;
        if (fetchRecord(frn, extRecord)) {
            // Pass a dummy vector since we don't expect nested $ATTRIBUTE_LISTs.
            std::vector<std::uint64_t> dummy; 
            parseRecordForData(extRecord, dummy);
        } else {
            error = "failed to read MFT extension record " + std::to_string(frn);
            return false;
        }
    }

    if (dataBlobs.empty()) {
        error = "no $DATA runs found in MFT records";
        return false;
    }

    // Sort extents by Lowest VCN to stitch them sequentially
    std::sort(dataBlobs.begin(), dataBlobs.end(), [](const DataExtentBlob& a, const DataExtentBlob& b) {
        return a.lowestVcn < b.lowestVcn;
    });

    std::int64_t prevLcn = 0;
    for (const auto& blob : dataBlobs) {
        std::vector<detail::DataRun> runs{};
        detail::parseDataRuns(blob.runsBytes.data(), blob.runsBytes.size(), runs);
        
        std::uint64_t currentVcn = blob.lowestVcn;
        for (const auto& run : runs) {
            if (!run.sparse && run.lengthClusters > 0) {
                // For extension records, the first run's delta is relative to the last LCN 
                // of the preceding attribute extent. Our parseDataRuns assumes base 0, 
                // so we just add prevLcn to correct it.
                std::int64_t realLcn = run.startLcn + prevLcn;
                MftExtent e{};
                e.fileOffset = currentVcn * clusterSize;
                e.diskOffset = static_cast<std::uint64_t>(realLcn) * clusterSize;
                e.length = run.lengthClusters * clusterSize;
                out.push_back(e);
            }
            currentVcn += run.lengthClusters;
        }
        
        // Find the last non-sparse run to update prevLcn for the next attribute extent
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            if (!it->sparse) {
                prevLcn += it->startLcn;
                break;
            }
        }
    }

    if (out.empty()) {
        error = "no usable $DATA runs parsed";
        return false;
    }

    // Sort the final physical extents by fileOffset for reading
    std::sort(out.begin(), out.end(), [](const MftExtent& a, const MftExtent& b) {
        return a.fileOffset < b.fileOffset;
    });

    return true;
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
    // The cache key is the drive letter, so entries are validated against
    // the live volume serial: a remount/reformat/USB-swap changes the
    // serial and the stale image is reloaded. Volumes are effectively
    // immutable during a scan; a rescan reloads only when the weak
    // reference has expired or the serial differs.
    DWORD currentSerial{0};
    {
        std::wstring volRoot = anyPathOnVolume.root_path().native();
        if (!volRoot.empty() && volRoot.back() != L'\\') {
            volRoot += L'\\';
        }
        GetVolumeInformationW(volRoot.c_str(), nullptr, 0, &currentSerial, nullptr, nullptr,
                              nullptr, 0);
    }
    static std::mutex cacheMutex{};
    static std::unordered_map<std::wstring, std::weak_ptr<SharedVolume>> cache{};
    {
        const std::lock_guard<std::mutex> lock{cacheMutex};
        const auto cached = cache.find(drive);
        if (cached != cache.end()) {
            if (auto shared = cached->second.lock()) {
                if (currentSerial != 0 && shared->volumeSerial == currentSerial) {
                    volume_ = std::move(shared);
                    return true;
                }
                cache.erase(cached); // stale mount or unknown serial: reload
            }
        }
    }
    if (volume_ && volume_->drivePrefix == drive && currentSerial != 0
        && volume_->volumeSerial == currentSerial) {
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
    // Reference length for the walk below: the extents must cover at least
    // the MFT's valid data, otherwise the walk stopped early.
    std::uint64_t mftValidLength{0};
    {
        NTFS_VOLUME_DATA_BUFFER volData{};
        DWORD volReturned{0};
        if (DeviceIoControl(volume.h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volData,
                            sizeof(volData), &volReturned, nullptr)) {
            mftValidLength = static_cast<std::uint64_t>(volData.MftValidDataLength.QuadPart);
        }
    }
    std::vector<MftExtent> extents{};
    std::string extentError{};
    if (!getMftExtents(volume.h, clusterSize, extents, extentError)) {
        lastError_ = "retrieval pointers (MFT extents): " + extentError;
        return false;
    }
    std::uint64_t imageEnd{0};
    for (const auto& e : extents) {
        // Span must cover holes: sparse runs advance the VCN walk without
        // emitting extents, so later fileOffsets can exceed the summed
        // lengths. Sizing by the sum would overflow the image buffer
        // (heap corruption, fail-fast crash). VirtualAlloc returns zeroed
        // pages, which is exactly what sparse clusters contain — holes
        // stay zero and parse-skipped via bad magic.
        imageEnd = (std::max)(imageEnd, e.fileOffset + e.length);
    }
    if (imageEnd == 0 || imageEnd % recordSize != 0) {
        return fail("implausible MFT size");
    }
    if (imageEnd > kMaxVolumeBytes) {
        // Not an error code situation: the volume is fine, the image just
        // exceeds the 2 GiB cap. Report plainly; the engine falls back to
        // native enumeration.
        lastError_ = "MFT image exceeds 2 GiB cap; using native enumeration";
        return false;
    }
    if (mftValidLength > 0 && imageEnd < mftValidLength) {
        return fail("truncated extent walk (below valid data length)");
    }

    // High-performance asynchronous MFT reading via multiple threads and OVERLAPPED I/O.
    VirtualImage imageMem{VirtualAlloc(nullptr, static_cast<SIZE_T>(imageEnd),
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    if (!imageMem) {
        return fail("image allocation (VirtualAlloc)");
    }
    unsigned char* image = static_cast<unsigned char*>(imageMem.get());

    struct ReadTask {
        std::uint64_t diskOffset;
        std::uint64_t fileOffset;
        DWORD length;
    };
    std::vector<ReadTask> readTasks;
    for (const auto& e : extents) {
        std::uint64_t done{0};
        while (done < e.length) {
            const DWORD step = static_cast<DWORD>((std::min)(e.length - done, static_cast<std::uint64_t>(kReadChunk)));
            readTasks.push_back({e.diskOffset + done, e.fileOffset + done, step});
            done += step;
        }
    }

    RaiiHandle asyncVol{CreateFileW(volumePath.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr)};
    if (asyncVol.h == INVALID_HANDLE_VALUE) {
        return fail("async volume open");
    }

    std::atomic<std::size_t> taskIdx{0};
    std::atomic<bool> readFailed{false};
    std::string readErrorMsg;
    std::mutex readErrorMutex;

    auto readWorker = [&]() {
        RaiiHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (event.h == nullptr) {
            readFailed.store(true, std::memory_order_relaxed);
            return;
        }
        
        while (true) {
            std::size_t i = taskIdx.fetch_add(1, std::memory_order_relaxed);
            if (i >= readTasks.size() || readFailed.load(std::memory_order_relaxed)) {
                break;
            }
            const auto& task = readTasks[i];
            
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(task.diskOffset & 0xFFFFFFFF);
            overlapped.OffsetHigh = static_cast<DWORD>(task.diskOffset >> 32);
            overlapped.hEvent = event.h;

            DWORD bytesRead = 0;
            if (ReadFile(asyncVol.h, image + task.fileOffset, task.length, &bytesRead, &overlapped) == 0) {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    readFailed.store(true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(readErrorMutex);
                    readErrorMsg = "MFT extent read (ReadFile err=" + std::to_string(err) + ")";
                    break;
                }
                
                DWORD waitResult;
                while ((waitResult = WaitForSingleObject(event.h, 50)) == WAIT_TIMEOUT) {
                    if (readFailed.load(std::memory_order_relaxed)) {
                        CancelIoEx(asyncVol.h, &overlapped);
                        break;
                    }
                }
                
                if (waitResult == WAIT_OBJECT_0) {
                    if (!GetOverlappedResult(asyncVol.h, &overlapped, &bytesRead, TRUE) || bytesRead != task.length) {
                        readFailed.store(true, std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lock(readErrorMutex);
                        readErrorMsg = "MFT extent read (GetOverlappedResult err=" + std::to_string(GetLastError()) + ")";
                        break;
                    }
                }
            } else if (bytesRead != task.length) {
                readFailed.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(readErrorMutex);
                readErrorMsg = "MFT extent read (incomplete sync read)";
                break;
            }
            ResetEvent(event.h);
        }
    };

    unsigned ioThreads = std::thread::hardware_concurrency();
    if (ioThreads == 0) ioThreads = 4;
    ioThreads = (std::min)(ioThreads, kMaxParseThreads);
    
    std::vector<std::thread> readThreads;
    readThreads.reserve(ioThreads);
    for (unsigned t = 0; t < ioThreads; ++t) {
        readThreads.emplace_back(readWorker);
    }
    for (auto& w : readThreads) {
        w.join();
    }

    if (readFailed.load()) {
        lastError_ = readErrorMsg;
        return false;
    }

    // Parallel record parsing (stride across threads for even load).
    const std::size_t count = static_cast<std::size_t>(imageEnd / recordSize);
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

    auto shared = std::make_shared<SharedVolume>();
    shared->drivePrefix = drive;
    shared->volumeSerial = currentSerial;
    shared->bytesPerCluster = clusterSize;
    for (auto& vec : local) {
        for (auto& rec : vec) {
            if (rec.usesClusterMath) {
                rec.physicalSize = rec.residentBytes + (rec.nonResidentClusters * clusterSize);
                rec.usesClusterMath = false;
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

bool MftFinder::open(const std::filesystem::path& dir, std::uint64_t tokenHint)
{
    close();
    if (!loadVolume(dir)) {
        return false;
    }
    // O(1) fast path: the engine hands back the record number observed
    // during the parent's enumeration, skipping path resolution entirely.
    if (tokenHint > 0 && tokenHint <= 0xFFFFFFFFULL) {
        const auto hinted = volume_->byNumber.find(static_cast<std::uint32_t>(tokenHint));
        if (hinted != volume_->byNumber.end()
            && volume_->records[hinted->second].isDirectory) {
            return openRecord(hinted->second, dir);
        }
        // Stale or foreign token: fall through to path resolution.
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
                lastError_ = "path component not found in volume index";
                return false;
            }
        }
    }
    const auto recIt = volume_->byNumber.find(current);
    if (recIt == volume_->byNumber.end()) {
        lastError_ = "directory record missing from volume index";
        return false;
    }
    return openRecord(recIt->second, dir);
}

bool MftFinder::openRecord(std::size_t index, const std::filesystem::path& dir)
{
    if (!volume_->records[index].isDirectory) {
        lastError_ = "target is not a directory";
        return false;
    }
    openedRecord_ = volume_->records[index].number;
    openedPath_ = dir;
    const auto childIt = volume_->children.find(openedRecord_);
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
        cur.backendToken = rec.number;
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

bool MftFinder::open(const std::filesystem::path&, std::uint64_t)
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
