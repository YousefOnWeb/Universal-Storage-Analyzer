#pragma once

#include "BlockingQueue.h"
#include "DirectoryFinder.h"
#include "Finder.h"
#include "MftFinder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace usa::scanner {

// One discovered node, backend-neutral, ready for UI projection.
struct EngineNode {
    std::uint32_t id{0};
    std::uint32_t parentId{0};
    std::string nameUtf8{};
    std::uint64_t logicalSize{0};
    std::uint64_t physicalSize{0};
    std::uint64_t lastWriteTs{0};
    bool isDirectory{false};
};

// Absolute running totals for one directory. Emitted repeatedly while its
// subtree streams in; the last emission per directory is the final total.
// Consumers replace stored values (never add); the engine already walked
// the ancestor chain, so every level converges exactly.
struct EngineDirSummary {
    std::uint32_t nodeId{0};
    std::uint64_t logicalSize{0};
    std::uint64_t physicalSize{0};
    std::uint32_t fileCount{0};
    std::uint32_t folderCount{0};
};

struct EngineResult {
    std::uint64_t fileCount{0};
    std::uint64_t dirCount{0};
    std::uint64_t totalBytes{0};
    BackendKind backend{BackendKind::NativeDirectory};
    bool cancelled{false};
    std::uint64_t durationMs{0};
    std::uint64_t batchCount{0};
    std::uint64_t errorCount{0};
};

struct EngineCallbacks {
    std::function<void(std::vector<EngineNode>)> onBatch{};
    std::function<void(const EngineDirSummary&)> onDirSummary{};
    // Severity is decided at the source: fatal = the scan cannot proceed
    // (missing target); non-fatal = one item skipped, scan continues.
    std::function<void(const std::string&, bool fatal)> onError{};
    std::function<void(const std::string&)> onProgress{};
    // Machine-parsable telemetry, one JSON object per line: periodic
    // snapshots (at most ~4/sec engine-wide) plus one final summary.
    // Kept out of the hot path — never per file.
    std::function<void(const std::string&)> onTelemetry{};
};

// Multi-threaded scan orchestrator (Qt-free).
//
// Picks a backend per scan root (direct volume reading when elevated on a
// supported volume, native enumeration otherwise), feeds directory jobs
// through a BlockingQueue to a worker pool, and streams node batches plus
// directory summaries to the callbacks. All callback invocations are
// serialized; callbacks must not call back into the engine.
//
// Aggregation ownership: the engine accumulates per-directory totals and
// propagates them up the ancestor chain itself (atomics for globals,
// mutex-guarded maps for the tree). Consumers store exactly what they are
// given. run() blocks until the scan finishes or is cancelled.
//
// Single-flight: one run() at a time per instance. Concurrent runs would
// share the queue, accumulators, and directory maps; the Qt bridge guards
// this with a single-flight check (see LocalScannerWorker).
class ScanEngine {
public:
    static constexpr std::size_t kBatchNodes{128};
    static constexpr std::chrono::milliseconds kFlushInterval{100};
    static constexpr unsigned kMaxWorkers{16};

    ScanEngine();
    ~ScanEngine();
    ScanEngine(const ScanEngine&) = delete;
    ScanEngine& operator=(const ScanEngine&) = delete;

    EngineResult run(const std::filesystem::path& root, const EngineCallbacks& callbacks,
                     const std::string& scanId = {});

    // Safe from any thread, even while run() blocks another thread.
    // Pause/cancel only touch synchronisation primitives, so the UI may
    // invoke them with a direct connection (no event-loop round trip).
    void setPaused(bool paused);
    void requestCancel();

private:
    struct DirAcc {
        std::atomic_uint64_t logical{0};
        std::atomic_uint64_t physical{0};
        std::atomic_uint32_t files{0};
        std::atomic_uint32_t folders{0};
        DirAcc* parent{nullptr};
        std::uint32_t id{0};
    };

    struct DirJob {
        std::filesystem::path path{};
        DirAcc* acc{nullptr};
        // Opaque backend identity of this directory (volume record number
        // from the direct reader). Lets the backend skip path resolution.
        std::uint64_t token{0};
    };

    std::unique_ptr<Finder> makeFinder();
    void workerMain(const EngineCallbacks& callbacks);
    void emitBatch(const EngineCallbacks& callbacks, std::vector<EngineNode>& batch);
    void emitChain(const EngineCallbacks& callbacks, DirAcc* leafAcc);
    void emitSweep(const EngineCallbacks& callbacks);
    void emitTelemetry(const EngineCallbacks& callbacks, const char* phase);

    BlockingQueue<DirJob> queue_{};
    std::atomic_uint32_t nextId_{1};
    std::atomic_uint64_t fileCount_{0};
    std::atomic_uint64_t dirCount_{0};
    std::atomic_uint64_t totalBytes_{0};
    std::atomic_uint64_t batchCount_{0};
    std::atomic_uint64_t errorCount_{0};
    std::atomic_uint64_t progressTick_{0};
    std::atomic_uint64_t lastTelemetryMs_{0};
    std::string scanId_{}; // set by run() before workers spawn; read-only after
    std::chrono::steady_clock::time_point runStart_{};
    std::atomic_bool userCancelled_{false};
    BackendKind backend_{BackendKind::NativeDirectory};
    std::mutex emitMutex_{}; // serializes all callback invocations
    std::mutex dirsMutex_{}; // guards allDirs_
    std::vector<std::unique_ptr<DirAcc>> allDirs_{};
};

} // namespace usa::scanner
