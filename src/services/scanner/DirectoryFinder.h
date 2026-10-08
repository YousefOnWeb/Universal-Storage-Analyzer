#pragma once

#include "Finder.h"

#include <cstdint>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace usa::scanner {

// Native directory enumeration backend (the "basic" engine).
//
// Windows: batches entries with NtQueryDirectoryFile (loaded dynamically
// from ntdll, no import dependency) using large local buffers and small
// remote-share buffers. Both logical (EndOfFile) and allocated
// (AllocationSize) sizes come from the enumeration itself — no extra
// per-file syscalls.
// POSIX (macOS/Linux): readdir-style iteration with lstat for allocated
// blocks and modification times. Symlinks are reported, never followed.
class DirectoryFinder : public Finder {
public:
    DirectoryFinder();
    ~DirectoryFinder() override;

    bool open(const std::filesystem::path& dir, std::uint64_t tokenHint = 0) override;
    bool findNext() override;
    const DirEntry& current() const override { return current_; }
    void close() override;
    std::string lastError() const override { return lastError_; }

private:
#ifdef _WIN32
    bool refill();
    bool fillCurrent(const unsigned char* record);

    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::vector<unsigned char> buffer_{};
    std::filesystem::path dir_{};
    std::size_t offset_{0};
    unsigned long returned_{0};
    unsigned long infoClass_{38}; // FileIdFullDirectoryInformation, else 2
    bool first_{true};
    bool triedFallback_{false};
#else
    std::filesystem::directory_iterator it_{};
    std::filesystem::directory_iterator end_{};
    bool started_{false};
#endif
    DirEntry current_{};
    std::string lastError_{};
};

} // namespace usa::scanner
