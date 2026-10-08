#include "ScanEngine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace usa::scanner {
namespace {

// std::filesystem::path::u8string() yields char8_t units; the engine speaks
// UTF-8 std::string across its boundary, so convert explicitly.
std::string utf8Name(const std::filesystem::path& p)
{
    const std::filesystem::path base = p.filename().empty() ? p : p.filename();
    const auto raw = base.u8string();
    return {reinterpret_cast<const char*>(raw.data()), raw.size()};
}

// Full-path variant for diagnostics: error messages must show WHERE,
// not just the leaf name.
std::string utf8Path(const std::filesystem::path& p)
{
    const auto raw = p.u8string();
    return {reinterpret_cast<const char*>(raw.data()), raw.size()};
}

// ISO-8601 UTC with fractional seconds for machine-parsable log lines.
// Resolution is OS-provided (often milliseconds); the field always carries
// microseconds of whatever the clock actually resolved.
std::string isoUtcNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto micros = duration_cast<microseconds>(now.time_since_epoch());
    const std::time_t secs = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&tm, &secs);
#endif
    char date[32]{};
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
    const auto frac = static_cast<unsigned long long>(micros.count() % 1000000LL);
    char out[48]{};
    std::snprintf(out, sizeof(out), "%s.%06lluZ", date, frac);
    return {out};
}

const char* backendName(BackendKind backend)
{
    return backend == BackendKind::DirectMft ? "DirectMft" : "NativeDirectory";
}

} // namespace

ScanEngine::ScanEngine() = default;
ScanEngine::~ScanEngine()
{
    queue_.requestCancel();
}

void ScanEngine::setPaused(bool paused)
{
    queue_.setSuspended(paused);
}

void ScanEngine::requestCancel()
{
    userCancelled_.store(true, std::memory_order_relaxed);
    queue_.requestCancel();
}

std::unique_ptr<Finder> ScanEngine::makeFinder()
{
    if (backend_ == BackendKind::DirectMft) {
        return std::make_unique<MftFinder>();
    }
    return std::make_unique<DirectoryFinder>();
}

void ScanEngine::emitBatch(const EngineCallbacks& callbacks, std::vector<EngineNode>& batch)
{
    if (batch.empty() || !callbacks.onBatch) {
        return;
    }
    std::vector<EngineNode> out{};
    out.swap(batch);
    std::lock_guard<std::mutex> lock{emitMutex_};
    callbacks.onBatch(std::move(out));
}

void ScanEngine::emitChain(const EngineCallbacks& callbacks, DirAcc* leafAcc)
{
    if (!callbacks.onDirSummary || !leafAcc) {
        return;
    }
    std::vector<EngineDirSummary> chain{};
    DirAcc* current = leafAcc;
    while (current != nullptr) {
        EngineDirSummary summary{};
        summary.nodeId = current->id;
        summary.logicalSize = current->logical.load(std::memory_order_relaxed);
        summary.physicalSize = current->physical.load(std::memory_order_relaxed);
        summary.fileCount = current->files.load(std::memory_order_relaxed);
        summary.folderCount = current->folders.load(std::memory_order_relaxed);
        chain.push_back(summary);
        current = current->parent;
    }
    std::lock_guard<std::mutex> lock{emitMutex_};
    for (const auto& s : chain) {
        callbacks.onDirSummary(s);
    }
}

void ScanEngine::emitTelemetry(const EngineCallbacks& callbacks, const char* phase)
{
    if (!callbacks.onTelemetry) {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                              - runStart_)
            .count();
    const auto files = fileCount_.load(std::memory_order_relaxed);
    const auto dirs = dirCount_.load(std::memory_order_relaxed);
    const auto nodes = files + dirs;
    const double rate =
        elapsed > 0 ? static_cast<double>(nodes) * 1000.0 / static_cast<double>(elapsed) : 0.0;
    const auto threadHash = static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::ostringstream json{};
    json << "{\"timestamp\":\"" << isoUtcNow() << "\",\"level\":\"INFO\""
         << ",\"scan_id\":\"" << scanId_ << "\",\"thread\":" << threadHash
         << ",\"subsystem\":\"ScanEngine/" << backendName(backend_) << '/' << phase << '"'
         << ",\"elapsed_ms\":" << elapsed << ",\"files\":" << files << ",\"dirs\":" << dirs
         << ",\"bytes\":" << totalBytes_.load(std::memory_order_relaxed)
         << ",\"nodes_per_sec\":" << std::fixed << std::setprecision(1) << rate
         << ",\"batches\":" << batchCount_.load(std::memory_order_relaxed)
         << ",\"errors\":" << errorCount_.load(std::memory_order_relaxed)
         << ",\"queue_depth\":" << queue_.size()
         << ",\"idle_workers\":" << queue_.waitingWorkers()
         << ",\"backend\":\"" << backendName(backend_) << "\"}";
    std::lock_guard<std::mutex> lock{emitMutex_};
    callbacks.onTelemetry(json.str());
}

void ScanEngine::emitLeaf(const EngineCallbacks& callbacks, DirAcc* acc)
{
    if (!callbacks.onDirSummary || acc == nullptr) {
        return;
    }
    EngineDirSummary summary{};
    summary.nodeId = acc->id;
    summary.logicalSize = acc->logical.load(std::memory_order_relaxed);
    summary.physicalSize = acc->physical.load(std::memory_order_relaxed);
    summary.fileCount = acc->files.load(std::memory_order_relaxed);
    summary.folderCount = acc->folders.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock{emitMutex_};
    callbacks.onDirSummary(summary);
}

void ScanEngine::emitSweep(const EngineCallbacks& callbacks)
{
    if (!callbacks.onDirSummary) {
        return;
    }
    // Absolute finals for every directory. Guarantees exactness no matter
    // which intermediate summaries were throttled away above. Runs after
    // all workers joined, so no accumulator can change underfoot.
    std::lock_guard<std::mutex> lock{emitMutex_};
    for (const auto& acc : allDirs_) {
        EngineDirSummary summary{};
        summary.nodeId = acc->id;
        summary.logicalSize = acc->logical.load(std::memory_order_relaxed);
        summary.physicalSize = acc->physical.load(std::memory_order_relaxed);
        summary.fileCount = acc->files.load(std::memory_order_relaxed);
        summary.folderCount = acc->folders.load(std::memory_order_relaxed);
        callbacks.onDirSummary(summary);
    }
}

void ScanEngine::workerMain(const EngineCallbacks& callbacks)
{
    auto finder = makeFinder();
    std::vector<EngineNode> batch{};
    batch.reserve(kBatchNodes);
    std::vector<std::unique_ptr<DirAcc>> localDirs{};
    auto lastFlush = std::chrono::steady_clock::now();
    const auto maybeFlush = [&](DirAcc* chainLeaf, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!batch.empty()
            && (force || batch.size() >= kBatchNodes || now - lastFlush >= kFlushInterval)) {
            // Time-based flushes refresh whole ancestor chains (live view);
            // size/forced flushes report just the finished job (cheap).
            // Either way the final sweep at run end guarantees exactness.
            const bool timed = !force && (now - lastFlush >= kFlushInterval);
            emitBatch(callbacks, batch);
            batchCount_.fetch_add(1, std::memory_order_relaxed);
            if (timed) {
                emitChain(callbacks, chainLeaf);
            } else {
                emitLeaf(callbacks, chainLeaf);
            }
            lastFlush = now;
            // Periodic telemetry, at most every 250 ms engine-wide. The
            // compare-exchange lets exactly one worker win each window;
            // a rare double emission is harmless for telemetry.
            const auto ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count());
            auto last = lastTelemetryMs_.load(std::memory_order_relaxed);
            if (ms - last >= 250
                && lastTelemetryMs_.compare_exchange_weak(last, ms, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
                emitTelemetry(callbacks, "periodic");
            }
        }
    };

    DirJob job{};
    while (queue_.pop(job)) {
        if (!finder->open(job.path, job.token)) {
            errorCount_.fetch_add(1, std::memory_order_relaxed);
            if (callbacks.onError) {
                std::string detail = finder->lastError();
                if (!detail.empty()) {
                    detail = " [" + detail + "]";
                }
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onError("Cannot enumerate directory: " + utf8Path(job.path) + detail,
                                  false);
            }
            queue_.taskDone();
            continue;
        }
        while (finder->findNext()) {
            queue_.waitIfSuspended();
            if (queue_.isCancelRequested()) {
                break;
            }
            const DirEntry& entry = finder->current();
            EngineNode node{};
            node.id = nextId_.fetch_add(1, std::memory_order_relaxed);
            node.parentId = job.acc ? job.acc->id : 0;
            node.nameUtf8 = utf8Name(entry.fullPath);
            node.logicalSize = entry.logicalSize;
            node.physicalSize = entry.physicalSize;
            node.lastWriteTs = entry.lastWriteTs;
            node.isDirectory = entry.isDirectory;

            if (entry.isDirectory && !entry.isReparsePoint) {
                auto childAcc = std::make_unique<DirAcc>();
                childAcc->parent = job.acc;
                childAcc->id = node.id;
                DirAcc* childAccPtr = childAcc.get();
                
                DirAcc* up = job.acc;
                while (up != nullptr) {
                    up->folders.fetch_add(1, std::memory_order_relaxed);
                    up = up->parent;
                }
                
                localDirs.push_back(std::move(childAcc));

                batch.push_back(std::move(node));
                dirCount_.fetch_add(1, std::memory_order_relaxed);
                queue_.push(DirJob{entry.fullPath, childAccPtr, entry.backendToken});
            } else {
                DirAcc* up = job.acc;
                while (up != nullptr) {
                    up->logical.fetch_add(entry.logicalSize, std::memory_order_relaxed);
                    up->physical.fetch_add(entry.physicalSize, std::memory_order_relaxed);
                    up->files.fetch_add(1, std::memory_order_relaxed);
                    up = up->parent;
                }

                batch.push_back(std::move(node));
                fileCount_.fetch_add(1, std::memory_order_relaxed);
                totalBytes_.fetch_add(entry.logicalSize, std::memory_order_relaxed);
            }
            if (callbacks.onProgress
                && progressTick_.fetch_add(1, std::memory_order_relaxed) % 2000 == 0) {
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onProgress(utf8Name(entry.fullPath));
            }
            maybeFlush(job.acc, false);
        }
        finder->close();
        maybeFlush(job.acc, true);
        queue_.taskDone();
    }
    maybeFlush(nullptr, true);

    if (!localDirs.empty()) {
        std::lock_guard<std::mutex> lock{dirsMutex_};
        for (auto& d : localDirs) {
            allDirs_.push_back(std::move(d));
        }
    }
}

EngineResult ScanEngine::run(const std::filesystem::path& root, const EngineCallbacks& callbacks,
                             const std::string& scanId)
{
    nextId_.store(1, std::memory_order_relaxed);
    fileCount_.store(0, std::memory_order_relaxed);
    dirCount_.store(0, std::memory_order_relaxed);
    totalBytes_.store(0, std::memory_order_relaxed);
    progressTick_.store(0, std::memory_order_relaxed);
    batchCount_.store(0, std::memory_order_relaxed);
    errorCount_.store(0, std::memory_order_relaxed);
    lastTelemetryMs_.store(0, std::memory_order_relaxed);
    userCancelled_.store(false, std::memory_order_relaxed);
    scanId_ = scanId; // read-only for the rest of the run (workers spawn below)
    
    auto startTime = std::chrono::steady_clock::now();
    runStart_ = startTime;
    queue_.reset();
    DirAcc* rootAcc = nullptr;
    {
        std::lock_guard<std::mutex> lock{dirsMutex_};
        allDirs_.clear();
        auto rAcc = std::make_unique<DirAcc>();
        rAcc->id = 0;
        rootAcc = rAcc.get();
        allDirs_.push_back(std::move(rAcc));
    }

    std::error_code ec{};
    if (!std::filesystem::exists(root, ec) || ec) {
        if (callbacks.onError) {
            callbacks.onError("Target path does not exist: " + utf8Path(root), true);
        }
        backend_ = BackendKind::NativeDirectory;
        return EngineResult{};
    }
    if (!std::filesystem::is_directory(root, ec) || ec) {
        if (callbacks.onError) {
            callbacks.onError("Target is not a directory: " + utf8Path(root), true);
        }
        backend_ = BackendKind::NativeDirectory;
        return EngineResult{};
    }

#ifdef _WIN32
    backend_ = isDirectScanAvailable(root) ? BackendKind::DirectMft : BackendKind::NativeDirectory;
#else
    backend_ = BackendKind::NativeDirectory;
#endif
    if (backend_ == BackendKind::DirectMft) {
        MftFinder probe{};
        if (!probe.open(root)) {
            backend_ = BackendKind::NativeDirectory;
            if (callbacks.onError) {
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onError("Direct volume reading unavailable; using native enumeration.",
                                  false);
            }
        }
    }

    unsigned workers = std::thread::hardware_concurrency();
    if (workers < 2U) {
        workers = 2U;
    }
    workers = (std::min)(workers, kMaxWorkers);

    queue_.push(DirJob{root, rootAcc, 0});
    std::vector<std::thread> pool{};
    for (unsigned i{0}; i < workers; ++i) {
        pool.emplace_back([this, &callbacks] { workerMain(callbacks); });
    }
    queue_.waitUntilIdle();
    queue_.requestCancel();
    for (auto& t : pool) {
        t.join();
    }
    emitSweep(callbacks);
    auto endTime = std::chrono::steady_clock::now();
    
    EngineResult result{};
    result.fileCount = fileCount_.load(std::memory_order_relaxed);
    result.dirCount = dirCount_.load(std::memory_order_relaxed);
    result.totalBytes = totalBytes_.load(std::memory_order_relaxed);
    result.backend = backend_;
    result.cancelled = userCancelled_.load(std::memory_order_relaxed);
    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    result.batchCount = batchCount_.load(std::memory_order_relaxed);
    result.errorCount = errorCount_.load(std::memory_order_relaxed);
    emitTelemetry(callbacks, "final");
    return result;
}

} // namespace usa::scanner
