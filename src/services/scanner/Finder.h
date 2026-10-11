#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace usa::scanner {

// One enumerated child of a directory, in backend-neutral form. Every
// backend (direct volume parsing, native directory enumeration, future
// remote/cloud readers) produces these; callers never know which backend
// is active.
struct DirEntry {
    std::filesystem::path fullPath{};
    bool isDirectory{false};
    // Reparse points (symlinks, junctions, mount points): listed but never
    // descended into, so cyclic graphs cannot trap the scanner.
    bool isReparsePoint{false};
    std::uint64_t logicalSize{0};
    std::uint64_t physicalSize{0}; // allocated on disk; best effort per backend
    std::uint64_t lastWriteTs{0}; // Unix seconds, 0 when unknown
    // Backend-opaque owner identity (POSIX uid string, volume owner id),
    // empty when the backend cannot provide it for free. Display-name
    // resolution happens later, off the hot path.
    std::string ownerId{};
    // Backend-opaque identity of this entry, enabling O(1) re-opening
    // without path resolution (e.g. the volume record number when produced
    // by the direct reader). Zero when the backend offers none.
    std::uint64_t backendToken{0};
    // Set by the direct volume reader when the record's own size fields
    // were zero/stale and a live native stat supplied the logical size
    // instead (spec 004b: locked system files, multi-extent streams).
    // Always false from every other backend.
    bool sizeRestated{false};
};

enum class BackendKind { NativeDirectory, DirectMft };

// One enumeration context. Instances are deliberately NOT thread-safe: the
// engine hands each worker its own Finder (per-thread context isolation),
// so backends need no locking on the hot path.
class Finder {
public:
    virtual ~Finder() = default;
    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    // Open a directory for enumeration. Returns false when the directory
    // cannot be enumerated (access denied, vanished, unsupported).
    // tokenHint carries a backendToken from a previous enumeration for O(1)
    // re-opening; 0 (or an unknown token) falls back to path resolution.
    virtual bool open(const std::filesystem::path& dir, std::uint64_t tokenHint = 0) = 0;
    // Advance to the next child. Returns false at end of directory.
    virtual bool findNext() = 0;
    // Valid after findNext() returns true, until the next findNext().
    virtual const DirEntry& current() const = 0;
    virtual void close() = 0;
    // OS-level reason for the most recent open() failure, e.g.
    // "access denied (win32=5)". Empty when unknown or not applicable.
    // Exists so diagnostics carry exact codes instead of guesses.
    virtual std::string lastError() const { return {}; }

protected:
    Finder() = default;
};

} // namespace usa::scanner
