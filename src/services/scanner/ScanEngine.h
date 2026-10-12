#pragma once

#include "BlockingQueue.h"
#include "Analytics.h"
#include "DirectoryFinder.h"
#include "Finder.h"
#include "MftFinder.h"
#include "ThrottlePolicy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
    // Backend-opaque owner identity, copied from the entry (see Finder.h).
    std::string ownerId{};
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

// One emitted file with logicalSize == 0. Zero can be legitimate
// (empty files) or a size-source failure — the audit names each one
// with its backend and physical size so the operator can tell.
struct ZeroLogicalEntry {
    std::string pathUtf8{};
    std::string backend{};
    std::uint64_t physicalSize{0};
    bool isReparse{false};
    bool ownerEmpty{true};
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
    // Phase breakdown for scan-speed diagnosis (all in run() wall time):
    // volume image load + parse + index, and the final summary sweep.
    // Traverse time = durationMs - loadMs - sweepMs.
    std::uint64_t loadMs{0};
    std::uint64_t sweepMs{0};
    // Size-audit (spec 004a, instrumentation only — no behavior change):
    // zero-logical files observed, reparse listings skipped from descent,
    // live chain reports emitted, sweep-vs-chain convergence deltas.
    std::uint64_t zeroLogicalCount{0};
    std::uint64_t reparseSkipped{0};
    std::uint64_t chainReports{0};
    std::uint64_t sweepDirs{0};
    std::uint64_t sweepChangedDirs{0};
    std::uint64_t sweepDeltaBytes{0};
    // Capped sample of zero-logical files (path, backend, sizes).
    std::vector<ZeroLogicalEntry> zeroEntries{};
    // Files whose zero/stale record size was replaced by a live native
    // stat (spec 004b). Counted, not listed (they are no longer zero).
    std::uint64_t restatFixed{0};
    // Extra directory rows from multi-link records (hardlinks, spec
    // 004c): child links beyond one per record, from the probe load.
    std::uint64_t multiNameExtra{0};
    // Root subtree-max last-write (Unix seconds, audit only; display
    // stays object mtime in this spec).
    std::uint64_t rootMaxMtime{0};
};

// Size-audit entry list accessor lives on the engine (see above).

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
    // Throttle transitions for status display. Emitted at most on change,
    // through the same serialized path as the other callbacks.
    std::function<void(bool throttled)> onThrottleChanged{};
    // Informational notices (backend downgrades, policy decisions): NOT
    // errors. They must never enter the error channel, where the UI would
    // collect them as skipped directories.
    std::function<void(const std::string&)> onNotice{};
    // Analytics snapshots (extensions, top files, age buckets, owners):
    // absolute replace-semantics values, live at most ~1/sec plus one
    // exact final after the sweep. Empty when the consumer wants none.
    std::function<void(const AnalyticsSnapshot&)> onAnalytics{};
    // Final directory totals as ONE bulk delivery (hundreds of thousands
    // of rows): the consumer applies values and repaints once instead of
    // choking the event loop with per-directory queued signals. Live
    // (possibly partial) totals still stream via onDirSummary.
    std::function<void(std::vector<EngineDirSummary>)> onSweep{};
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
    // Batch cadence: large enough that queued delivery + per-group model
    // inserts amortize (measured: 7-node average batches drowned the GUI
    // thread in 260k+ transactions on a full-volume scan), small enough
    // that the UI still streams live several times per second.
    static constexpr std::size_t kBatchNodes{512};
    static constexpr std::chrono::milliseconds kFlushInterval{250};
    static constexpr unsigned kMaxWorkers{16};

    ScanEngine();
    ~ScanEngine();
    ScanEngine(const ScanEngine&) = delete;
    ScanEngine& operator=(const ScanEngine&) = delete;

    EngineResult run(const std::filesystem::path& root, const EngineCallbacks& callbacks,
                     const std::string& scanId = {});

    // Size-audit entries (capped): files emitted with logicalSize == 0.
    // Snapshot after run() returns; valid until the next run().
    std::vector<ZeroLogicalEntry> zeroLogicalEntries() const;

    // Safe from any thread, even while run() blocks another thread.
    // Pause/cancel only touch synchronisation primitives, so the UI may
    // invoke them with a direct connection (no event-loop round trip).
    void setPaused(bool paused);
    void requestCancel();
    // Throttle control, same thread-safety contract: only atomics plus
    // the permit condition wake. The sampler (or tests) may call these
    // directly from any thread, mid-scan.
    void setWorkerCap(unsigned n);
    unsigned workerCap() const;
    void applyThrottleSample(const ThrottleInput& sample);
    // Master switch for disk-driven throttling. Disabling resets the
    // policy and restores the full pool; enabling just arms it.
    void setThrottleEnabled(bool enabled);
    // Analytics configuration. Pre-run only (single-flight engine).
    void setAgeBuckets(std::vector<std::uint64_t> days);
    void setTopN(std::size_t n);

private:
    struct DirAcc {
        std::atomic_uint64_t logical{0};
        std::atomic_uint64_t physical{0};
        std::atomic_uint32_t files{0};
        std::atomic_uint32_t folders{0};
        std::atomic_uint64_t maxMtime{0}; // subtree-max last-write (audit only)
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
    void workerMain(const EngineCallbacks& callbacks, unsigned index);
    void waitForPermit(unsigned index);
    void emitBatch(const EngineCallbacks& callbacks, std::vector<EngineNode>& batch);
    void emitChain(const EngineCallbacks& callbacks, DirAcc* leafAcc);
    void emitSweep(const EngineCallbacks& callbacks);
    void emitTelemetry(const EngineCallbacks& callbacks, const char* phase);
    void emitAnalytics(const EngineCallbacks& callbacks);

    BlockingQueue<DirJob> queue_{};
    Analytics analytics_{};
    std::atomic_uint32_t nextId_{1};
    std::atomic_uint64_t fileCount_{0};
    std::atomic_uint64_t dirCount_{0};
    std::atomic_uint64_t totalBytes_{0};
    std::atomic_uint64_t batchCount_{0};
    std::atomic_uint64_t errorCount_{0};
    std::atomic_uint64_t progressTick_{0};
    std::atomic_uint64_t lastTelemetryMs_{0};
    std::atomic_uint64_t lastAnalyticsMs_{0};
    std::string scanId_{}; // set by run() before workers spawn; read-only after
    std::chrono::steady_clock::time_point runStart_{};
    std::uint64_t runStartUnix_{0}; // system seconds at run start, for age math
    std::uint64_t loadMs_{0}; // volume load wall time (set before pool spawn)
    std::uint64_t sweepMs_{0}; // final sweep wall time (set after join)
    std::atomic_bool userCancelled_{false};
    BackendKind backend_{BackendKind::NativeDirectory};
    std::mutex emitMutex_{}; // serializes all callback invocations
    std::mutex dirsMutex_{}; // guards allDirs_
    std::vector<std::unique_ptr<DirAcc>> allDirs_{};
    // Size-audit state (spec 004a): counters are atomics (worker hot path),
    // entries + last-chain map are mutex-guarded (bounded, off the hot path).
    static constexpr std::size_t kMaxZeroEntries{2000};
    std::atomic_uint64_t zeroLogicalCount_{0};
    std::atomic_uint64_t reparseSkipped_{0};
    std::atomic_uint64_t chainReportCount_{0};
    std::atomic_uint64_t restatFixed_{0};
    std::atomic_uint64_t sweepChangedDirs_{0};
    std::atomic_uint64_t sweepDeltaBytes_{0};
    mutable std::mutex auditMutex_{};
    std::vector<ZeroLogicalEntry> zeroEntries_{};
    // Last live chain report per directory (for sweep convergence deltas).
    // Written under emitMutex_ (same serialization as all callbacks).
    std::unordered_map<std::uint32_t, EngineDirSummary> lastChain_{};
    ThrottlePolicy throttlePolicy_{};
    std::atomic_bool throttleEnabled_{true};
    std::atomic_uint workerCap_{kMaxWorkers};
    std::atomic_bool lastThrottleEmitted_{false};
    std::mutex capMutex_{};
    std::condition_variable capCv_{};
};

} // namespace usa::scanner
