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
};

struct EngineCallbacks {
    std::function<void(std::vector<EngineNode>)> onBatch{};
    std::function<void(const EngineDirSummary&)> onDirSummary{};
    std::function<void(const std::string&)> onError{};
    std::function<void(const std::string&)> onProgress{};
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
class ScanEngine {
public:
    static constexpr std::size_t kBatchNodes{128};
    static constexpr std::chrono::milliseconds kFlushInterval{100};
    static constexpr unsigned kMaxWorkers{16};

    ScanEngine();
    ~ScanEngine();
    ScanEngine(const ScanEngine&) = delete;
    ScanEngine& operator=(const ScanEngine&) = delete;

    EngineResult run(const std::filesystem::path& root, const EngineCallbacks& callbacks);

    // Safe from any thread, even while run() blocks another thread.
    // Pause/cancel only touch synchronisation primitives, so the UI may
    // invoke them with a direct connection (no event-loop round trip).
    void setPaused(bool paused);
    void requestCancel();

private:
    struct DirJob {
        std::filesystem::path path{};
        std::uint32_t id{0};
    };
    struct DirAcc {
        std::uint64_t logical{0};
        std::uint64_t physical{0};
        std::uint32_t files{0};
        std::uint32_t folders{0};
    };

    std::unique_ptr<Finder> makeFinder();
    void workerMain(const EngineCallbacks& callbacks);
    void emitBatch(const EngineCallbacks& callbacks, std::vector<EngineNode>& batch);
    void emitChain(const EngineCallbacks& callbacks, std::uint32_t leafId);

    BlockingQueue<DirJob> queue_{};
    std::atomic_uint32_t nextId_{1};
    std::atomic_uint64_t fileCount_{0};
    std::atomic_uint64_t dirCount_{0};
    std::atomic_uint64_t totalBytes_{0};
    std::atomic_uint64_t progressTick_{0};
    std::atomic_bool userCancelled_{false};
    BackendKind backend_{BackendKind::NativeDirectory};
    std::mutex emitMutex_{}; // serializes all callback invocations
    std::mutex mapMutex_{}; // guards parentOf_ and totals_
    std::unordered_map<std::uint32_t, std::uint32_t> parentOf_{};
    std::unordered_map<std::uint32_t, DirAcc> totals_{};
};

} // namespace usa::scanner
