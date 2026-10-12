#pragma once

#include "Finder.h"
#include "MftRecord.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace usa::scanner {

// Direct NTFS volume reader (Windows only; unavailable elsewhere).
//
// Reads the Master File Table straight from the volume device, so directory
// contents resolve from cached structures instead of per-file syscalls.
// Requires elevation and an NTFS volume — see isDirectScanAvailable().
// Any failure (access, parsing, validation) is reported as "unavailable";
// the engine then falls back to native directory enumeration.
//
// Threading: one MftFinder instance serves a single worker thread at a
// time (same per-thread isolation contract as every Finder). Volume data
// is loaded once and shared read-only by all instances via LoadShared().
class MftFinder : public Finder {
public:
    MftFinder();
    ~MftFinder() override;

    // One directory row: a record plus which of its $FILE_NAME links
    // names it here (hardlinked files appear once per link, spec 004c).
    // nameIdx indexes MftRecord::fileNames; UINT32_MAX selects the
    // legacy primary name (records parsed before links were kept).
    struct ChildRef {
        std::uint32_t number{0};
        std::uint32_t nameIdx{UINT32_MAX};
    };

    bool open(const std::filesystem::path& dir, std::uint64_t tokenHint = 0) override;
    bool findNext() override;
    const DirEntry& current() const override { return current_; }
    void close() override;

    // Load (or reuse, if already cached) the volume containing the path.
    // Cached images are validated against the live volume serial, so a
    // remount/reformat between scans triggers a reload. A target newer
    // than the image read also forces a reload, so scans never list a
    // stale directory tree.
    bool loadVolume(const std::filesystem::path& anyPathOnVolume);
    std::size_t recordCount() const;
    bool volumeLoaded() const;
    // Extra directory rows from multi-link records (hardlinks): total
    // child links minus one row per record. Zero when no volume loaded.
    std::uint64_t extraNameLinks() const;
    // Human-readable reason for the most recent loadVolume() failure
    // (step name + Win32 error). Empty when the last load succeeded.
    std::string lastError() const override { return lastError_; }

private:
    struct SharedVolume;
    bool openRecord(std::size_t index, const std::filesystem::path& dir);
    std::shared_ptr<SharedVolume> volume_{};
    std::filesystem::path openedPath_{};
    std::uint32_t openedRecord_{0};
    std::vector<ChildRef> children_{};
    std::size_t position_{0};
    bool enumerationStarted_{false};
    DirEntry current_{};
    std::string lastError_{};
};

// True on Windows when the process is elevated AND the path lives on an
// NTFS volume. Always false on other platforms.
bool isDirectScanAvailable(const std::filesystem::path& root);

#ifdef _WIN32
// Live single-file stat for zero-logical volume records (spec 004b).
// GetFileAttributesEx opens nothing, so exclusive locks (paging,
// hibernation, registry hives) do not matter. Returns true with the
// live logical size; false leaves the record values in place.
bool nativeRestatSize(const std::filesystem::path& p, std::uint64_t& sizeOut);

// Parent-directory enumeration for files even attribute stats cannot
// see (exclusively locked system files). Opens the parent only and
// matches by file name (case-insensitive, volume semantics). Returns
// true with the enumerated logical size.
bool nativeDirentSize(const std::filesystem::path& p, std::uint64_t& sizeOut);
#endif

// Expands per-name parent links: one ChildRef per real $FILE_NAME
// entry (DOS aliases suppressed when the record has any real name).
// Skips the volume root record. Returns extra rows beyond one per
// record (the hardlink count). Pure function for unit testing.
std::uint64_t buildChildLinks(const std::vector<detail::MftRecord>& records,
                              std::uint32_t rootNumber,
                              std::unordered_map<std::uint32_t, std::vector<MftFinder::ChildRef>>& out);

} // namespace usa::scanner
