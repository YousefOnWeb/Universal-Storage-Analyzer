#pragma once

#include "Finder.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

    bool open(const std::filesystem::path& dir) override;
    bool findNext() override;
    const DirEntry& current() const override { return current_; }
    void close() override;

    // Load (or reuse, if already cached) the volume containing the path.
    bool loadVolume(const std::filesystem::path& anyPathOnVolume);
    std::size_t recordCount() const;
    bool volumeLoaded() const;
    // Human-readable reason for the most recent loadVolume() failure
    // (step name + Win32 error). Empty when the last load succeeded.
    std::string lastError() const override { return lastError_; }

private:
    struct SharedVolume;
    std::shared_ptr<SharedVolume> volume_{};
    std::filesystem::path openedPath_{};
    std::uint32_t openedRecord_{0};
    std::vector<std::uint32_t> children_{};
    std::size_t position_{0};
    bool enumerationStarted_{false};
    DirEntry current_{};
    std::string lastError_{};
};

// True on Windows when the process is elevated AND the path lives on an
// NTFS volume. Always false on other platforms.
bool isDirectScanAvailable(const std::filesystem::path& root);

} // namespace usa::scanner
