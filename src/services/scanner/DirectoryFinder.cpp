#include "DirectoryFinder.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <system_error>

#ifdef _WIN32

namespace usa::scanner {
namespace {

// --- Minimal native-API declarations (loaded from ntdll at runtime) ---

struct UsaUnicodeString {
    USHORT Length{0};
    USHORT MaximumLength{0};
    wchar_t* Buffer{nullptr};
};

struct UsaIoStatusBlock {
    union {
        LONG Status{0};
        void* Pointer;
    };
    void* Information{nullptr};
};

using NtQueryDirectoryFileFn = LONG(__stdcall*)(
    HANDLE, HANDLE, void*, void*, UsaIoStatusBlock*, void*, ULONG, ULONG, BOOLEAN,
    UsaUnicodeString*, BOOLEAN);

NtQueryDirectoryFileFn queryFn()
{
    static NtQueryDirectoryFileFn fn = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        NtQueryDirectoryFileFn loaded{};
        if (ntdll != nullptr) {
            // memcpy (not reinterpret_cast) between the FARPROC and the
            // function pointer: avoids -Wcast-function-type and is the
            // strictly-correct way to convert a dlsym-style address.
            void* addr = reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
            static_assert(sizeof(loaded) == sizeof(addr));
            std::memcpy(&loaded, &addr, sizeof(loaded));
        }
        return loaded;
    }();
    return fn;
}

// FILE_ID_FULL_DIRECTORY_INFORMATION (class 38)
struct UsaFileIdFullDirInfo {
    ULONG NextEntryOffset{0};
    ULONG FileIndex{0};
    LARGE_INTEGER CreationTime{};
    LARGE_INTEGER LastAccessTime{};
    LARGE_INTEGER LastWriteTime{};
    LARGE_INTEGER ChangeTime{};
    LARGE_INTEGER EndOfFile{};
    LARGE_INTEGER AllocationSize{};
    ULONG FileAttributes{0};
    ULONG FileNameLength{0}; // bytes, not characters
    ULONG EaSize{0};
    LARGE_INTEGER FileId{};
    WCHAR FileName[1]{0};
};

// FILE_FULL_DIRECTORY_INFORMATION (class 2, fallback)
struct UsaFileFullDirInfo {
    ULONG NextEntryOffset{0};
    ULONG FileIndex{0};
    LARGE_INTEGER CreationTime{};
    LARGE_INTEGER LastAccessTime{};
    LARGE_INTEGER LastWriteTime{};
    LARGE_INTEGER ChangeTime{};
    LARGE_INTEGER EndOfFile{};
    LARGE_INTEGER AllocationSize{};
    ULONG FileAttributes{0};
    ULONG FileNameLength{0}; // bytes, not characters
    ULONG EaSize{0};
    WCHAR FileName[1]{0};
};

constexpr ULONG kFileIdFullDirInfoClass{38};
constexpr ULONG kFileFullDirInfoClass{2};
constexpr LONG kStatusNoMoreFiles{static_cast<LONG>(0x80000006UL)};
constexpr std::size_t kLocalBufferSize{4 * 1024 * 1024};
constexpr std::size_t kRemoteBufferSize{64 * 1024};

// Enumeration records are only DWORD-aligned inside the buffer, so member
// access through the struct pointer is unaligned access (strict UB, breaks
// under -Wcast-align / ARM). Read every field with memcpy; offsetof keeps
// the struct declarations as the single source of layout truth.
template <typename T> T readField(const unsigned char* p)
{
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::uint64_t filetimeToUnix(std::int64_t ft)
{
    constexpr std::uint64_t kEpochDiff{116444736000000000ULL};
    constexpr std::uint64_t kTicksPerSec{10000000ULL};
    const auto raw = static_cast<std::uint64_t>(ft);
    return raw > kEpochDiff ? (raw - kEpochDiff) / kTicksPerSec : 0;
}

std::wstring toLongPath(const std::filesystem::path& p)
{
    std::wstring s = p.native();
    // The \\?\ prefix passes the path literally to the filesystem, so
    // forward slashes (e.g. from Qt temp paths) must be normalized first.
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.starts_with(L"\\\\?\\") || s.starts_with(L"\\\\.\\")) {
        return s;
    }
    if (s.size() >= 2 && s[1] == L':') {
        return L"\\\\?\\" + s;
    }
    if (s.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    std::error_code ec{};
    const auto abs = std::filesystem::absolute(p, ec);
    if (ec || abs == p) {
        // Unresolvable: return the normalized input instead of recursing
        // on an identical path (which would overflow the stack).
        return s;
    }
    return toLongPath(abs);
}

} // namespace

DirectoryFinder::DirectoryFinder() = default;

DirectoryFinder::~DirectoryFinder()
{
    close();
}

bool DirectoryFinder::open(const std::filesystem::path& dir, std::uint64_t /*tokenHint*/)
{
    close();
    lastError_.clear();
    if (queryFn() == nullptr) {
        lastError_ = "NtQueryDirectoryFile unavailable";
        return false;
    }
    const std::wstring path = toLongPath(dir);
    HANDLE h = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        lastError_ = "open failed (win32=" + std::to_string(code) + ")";
        return false;
    }
    std::wstring root = dir.root_path().native();
    if (!root.empty() && root.back() != L'\\') {
        root += L'\\';
    }
    const bool remote = !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    handle_ = h;
    dir_ = dir;
    buffer_.assign(remote ? kRemoteBufferSize : kLocalBufferSize, 0);
    offset_ = 0;
    returned_ = 0;
    infoClass_ = kFileIdFullDirInfoClass;
    first_ = true;
    triedFallback_ = false;
    return true;
}

void DirectoryFinder::close()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    // Keep the buffer capacity: the next open() reuses it instead of
    // churning a multi-megabyte allocation per directory.
    buffer_.clear();
    offset_ = 0;
    returned_ = 0;
}

bool DirectoryFinder::refill()
{
    for (int attempt{0}; attempt < 2; ++attempt) {
        UsaIoStatusBlock ios{};
        const LONG status = queryFn()(
            handle_, nullptr, nullptr, nullptr, &ios, buffer_.data(),
            static_cast<ULONG>(buffer_.size()), infoClass_, static_cast<BOOLEAN>(0), nullptr,
            first_ ? static_cast<BOOLEAN>(1) : static_cast<BOOLEAN>(0));
        first_ = false;
        if (status == kStatusNoMoreFiles) {
            return false;
        }
        if (status == 0) {
            returned_ = static_cast<unsigned long>(
                reinterpret_cast<std::uintptr_t>(ios.Information));
            offset_ = 0;
            return returned_ > 0;
        }
        // Unknown file-ID support (or transient error): retry once with the
        // widely-supported information class.
        if (!triedFallback_ && infoClass_ == kFileIdFullDirInfoClass) {
            triedFallback_ = true;
            infoClass_ = kFileFullDirInfoClass;
            first_ = true;
            continue;
        }
        return false;
    }
    return false;
}

bool DirectoryFinder::fillCurrent(const unsigned char* record)
{
    std::wstring name{};
    std::uint64_t logical{0};
    std::uint64_t physical{0};
    std::int64_t writeFt{0};
    ULONG attributes{0};
    ULONG nameLenBytes{0};
    std::size_t nameOff{0};
    if (infoClass_ == kFileIdFullDirInfoClass) {
        attributes = readField<ULONG>(record + offsetof(UsaFileIdFullDirInfo, FileAttributes));
        logical = static_cast<std::uint64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileIdFullDirInfo, EndOfFile)));
        physical = static_cast<std::uint64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileIdFullDirInfo, AllocationSize)));
        writeFt = static_cast<std::int64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileIdFullDirInfo, LastWriteTime)));
        nameLenBytes =
            readField<ULONG>(record + offsetof(UsaFileIdFullDirInfo, FileNameLength));
        nameOff = offsetof(UsaFileIdFullDirInfo, FileName);
    } else {
        attributes = readField<ULONG>(record + offsetof(UsaFileFullDirInfo, FileAttributes));
        logical = static_cast<std::uint64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileFullDirInfo, EndOfFile)));
        physical = static_cast<std::uint64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileFullDirInfo, AllocationSize)));
        writeFt = static_cast<std::int64_t>(
            readField<LONGLONG>(record + offsetof(UsaFileFullDirInfo, LastWriteTime)));
        nameLenBytes = readField<ULONG>(record + offsetof(UsaFileFullDirInfo, FileNameLength));
        nameOff = offsetof(UsaFileFullDirInfo, FileName);
    }
    const std::size_t chars = nameLenBytes / sizeof(WCHAR);
    name.resize(chars);
    if (chars > 0) {
        std::memcpy(name.data(), record + nameOff, chars * sizeof(WCHAR));
    }
    if (name == L"." || name == L"..") {
        return false;
    }
    DirEntry cur{};
    cur.fullPath = dir_ / name;
    cur.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    cur.isReparsePoint = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (cur.isReparsePoint) {
        cur.isDirectory = false; // listed, never descended into
    }
    cur.logicalSize = cur.isDirectory ? 0 : logical;
    cur.physicalSize = cur.isDirectory ? 0 : physical;
    cur.lastWriteTs = filetimeToUnix(writeFt);
    current_ = std::move(cur);
    return true;
}

bool DirectoryFinder::findNext()
{
    if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    for (;;) {
        if (offset_ >= returned_) {
            if (!refill()) {
                return false;
            }
        }
        const unsigned char* record = buffer_.data() + offset_;
        ULONG next{0};
        if (infoClass_ == kFileIdFullDirInfoClass) {
            next = readField<ULONG>(record + offsetof(UsaFileIdFullDirInfo, NextEntryOffset));
        } else {
            next = readField<ULONG>(record + offsetof(UsaFileFullDirInfo, NextEntryOffset));
        }
        const bool usable = fillCurrent(record);
        offset_ = (next == 0) ? returned_ : offset_ + next;
        if (usable) {
            return true;
        }
    }
}

} // namespace usa::scanner

#else // POSIX (macOS / Linux)

#include <sys/stat.h>

namespace usa::scanner {

DirectoryFinder::DirectoryFinder() = default;
DirectoryFinder::~DirectoryFinder() = default;

bool DirectoryFinder::open(const std::filesystem::path& dir, std::uint64_t /*tokenHint*/)
{
    close();
    lastError_.clear();
    std::error_code ec{};
    it_ = std::filesystem::directory_iterator(dir, ec);
    if (ec) {
        it_ = std::filesystem::directory_iterator{};
        lastError_ = "opendir failed (errno=" + std::to_string(ec.value())
            + " " + ec.message() + ")";
        return false;
    }
    started_ = false;
    return true;
}

void DirectoryFinder::close()
{
    it_ = std::filesystem::directory_iterator{};
    started_ = false;
}

bool DirectoryFinder::findNext()
{
    std::error_code ec{};
    if (!started_) {
        started_ = true;
    } else {
        it_.increment(ec);
        if (ec) {
            return false;
        }
    }
    for (; it_ != end_; it_.increment(ec)) {
        if (ec) {
            return false;
        }
        const std::filesystem::path p = it_->path();
        const auto st = it_->symlink_status(ec);
        if (ec) {
            continue; // vanished mid-scan; skip, do not abort
        }
        const bool isLink = std::filesystem::is_symlink(st);
        const bool isReg = std::filesystem::is_regular(st);
        const bool isDir = std::filesystem::is_directory(st) && !isLink;

        DirEntry cur{};
        cur.fullPath = p;
        cur.isDirectory = isDir;
        cur.isReparsePoint = isLink;
        if (isReg && !isLink) {
            const auto sz = std::filesystem::file_size(p, ec);
            if (!ec) {
                cur.logicalSize = static_cast<std::uint64_t>(sz);
            }
        }
        struct ::stat sb {};
        if (::lstat(p.c_str(), &sb) == 0) {
            const auto blocks = sb.st_blocks > 0 ? sb.st_blocks : 0;
            cur.physicalSize =
                static_cast<std::uint64_t>(blocks) * static_cast<std::uint64_t>(512);
            cur.lastWriteTs = static_cast<std::uint64_t>(sb.st_mtime);
            cur.ownerId = std::to_string(sb.st_uid); // free with the stat call
        }
        if (cur.isReparsePoint) {
            cur.logicalSize = 0; // link target size is meaningless here
        }
        current_ = std::move(cur);
        return true;
    }
    return false;
}

} // namespace usa::scanner

#endif
