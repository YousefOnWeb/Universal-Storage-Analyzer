#include "ScanEngine.h"

#include "Utf8.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace usa::scanner {
namespace {

// Engine names/paths go through the infallible converter: volume names
// may hold unpaired surrogates that make std::filesystem converters throw,
// which would terminate the worker thread (and the process) on the spot.
std::string utf8Name(const std::filesystem::path& p)
{
    return detail::pathToUtf8(p, true);
}

// Full-path variant for diagnostics: error messages must show WHERE,
// not just the leaf name.
std::string utf8Path(const std::filesystem::path& p)
{
    return detail::pathToUtf8(p, false);
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
    capCv_.notify_all(); // wake threads parked in waitForPermit
}

void ScanEngine::setWorkerCap(unsigned n)
{
    if (n < 1U) {
        n = 1U;
    }
    if (n > kMaxWorkers) {
        n = kMaxWorkers;
    }
    workerCap_.store(n, std::memory_order_relaxed);
    capCv_.notify_all();
}

unsigned ScanEngine::workerCap() const
{
    return workerCap_.load(std::memory_order_relaxed);
}

void ScanEngine::applyThrottleSample(const ThrottleInput& sample)
{
    if (!throttleEnabled_.load(std::memory_order_relaxed)) {
        return;
    }
    throttlePolicy_.feed(sample);
    setWorkerCap(throttlePolicy_.throttled() ? 1U : kMaxWorkers);
}

void ScanEngine::setThrottleEnabled(bool enabled)
{
    throttleEnabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        throttlePolicy_.reset();
        setWorkerCap(kMaxWorkers);
    }
}

void ScanEngine::waitForPermit(unsigned index)
{
    // Parked threads hold no job: in-flight work always drains, and only
    // new dispatch waits. Cancel cuts the wait so joins cannot hang.
    std::unique_lock<std::mutex> lock{capMutex_};
    capCv_.wait(lock, [this, index] {
        return index < workerCap_.load(std::memory_order_relaxed)
               || queue_.isCancelRequested();
    });
}

void ScanEngine::setAgeBuckets(std::vector<std::uint64_t> days)
{
    analytics_.setAgeBuckets(std::move(days));
}

void ScanEngine::setTopN(std::size_t n)
{
    analytics_.setTopN(n);
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
         << ",\"load_ms\":" << loadMs_ << ",\"sweep_ms\":" << sweepMs_
         << ",\"queue_depth\":" << queue_.size()
         << ",\"idle_workers\":" << queue_.waitingWorkers()
         << ",\"throttled\":" << (throttlePolicy_.throttled() ? "true" : "false")
         << ",\"backend\":\"" << backendName(backend_) << "\"}";
    std::lock_guard<std::mutex> lock{emitMutex_};
    callbacks.onTelemetry(json.str());
}

void ScanEngine::emitSweep(const EngineCallbacks& callbacks)
{
    if (!callbacks.onSweep) {
        return;
    }
    // Absolute finals for every directory. Guarantees exactness no matter
    // which intermediate summaries were throttled away above. Runs after
    // all workers joined, so no accumulator can change underfoot. One bulk
    // event — not one queued signal per directory.
    std::vector<EngineDirSummary> out{};
    out.reserve(allDirs_.size());
    {
        std::lock_guard<std::mutex> lock{emitMutex_};
        for (const auto& acc : allDirs_) {
            EngineDirSummary summary{};
            summary.nodeId = acc->id;
            summary.logicalSize = acc->logical.load(std::memory_order_relaxed);
            summary.physicalSize = acc->physical.load(std::memory_order_relaxed);
            summary.fileCount = acc->files.load(std::memory_order_relaxed);
            summary.folderCount = acc->folders.load(std::memory_order_relaxed);
            out.push_back(summary);
        }
    }
    callbacks.onSweep(std::move(out));
}

void ScanEngine::emitAnalytics(const EngineCallbacks& callbacks)
{
    if (!callbacks.onAnalytics) {
        return;
    }
    // Snapshot under the analytics lock, deliver under the emit lock.
    // Lock order (emit -> analytics) is the only nesting anywhere:
    // recordFile() takes the analytics lock alone, so no inversion.
    AnalyticsSnapshot snap = analytics_.snapshot();
    std::lock_guard<std::mutex> lock{emitMutex_};
    callbacks.onAnalytics(std::move(snap));
}

void ScanEngine::workerMain(const EngineCallbacks& callbacks, unsigned index)
{
    auto finder = makeFinder();
    std::vector<EngineNode> batch{};
    batch.reserve(kBatchNodes);
    std::vector<std::unique_ptr<DirAcc>> localDirs{};
    // Per-worker aggregation: ancestor totals accumulate in a thread-local
    // map and merge into the shared atomics once per flush instead of once
    // per file (~500x fewer contended RMWs; deep trees hammer the shared
    // upper levels otherwise). Live summaries still converge: every merge
    // precedes the chain emission of the same flush.
    struct LocalAgg {
        std::uint64_t logical{0};
        std::uint64_t physical{0};
        std::uint32_t files{0};
        std::uint32_t folders{0};
    };
    std::unordered_map<DirAcc*, LocalAgg> localAgg{};
    std::uint64_t localFiles{0};
    std::uint64_t localDirsN{0};
    std::uint64_t localBytes{0};
    const auto mergeAgg = [&] {
        for (const auto& [acc, a] : localAgg) {
            acc->logical.fetch_add(a.logical, std::memory_order_relaxed);
            acc->physical.fetch_add(a.physical, std::memory_order_relaxed);
            acc->files.fetch_add(a.files, std::memory_order_relaxed);
            acc->folders.fetch_add(a.folders, std::memory_order_relaxed);
        }
        localAgg.clear();
        fileCount_.fetch_add(localFiles, std::memory_order_relaxed);
        localFiles = 0;
        dirCount_.fetch_add(localDirsN, std::memory_order_relaxed);
        localDirsN = 0;
        totalBytes_.fetch_add(localBytes, std::memory_order_relaxed);
        localBytes = 0;
    };
    auto lastFlush = std::chrono::steady_clock::now();
    const auto maybeFlush = [&](DirAcc* chainLeaf, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!batch.empty()
            && (force || batch.size() >= kBatchNodes || now - lastFlush >= kFlushInterval)) {
            mergeAgg();
            // Time-based flushes refresh whole ancestor chains (live view).
            // Size/forced flushes emit nodes only: their summaries would
            // duplicate what the final sweep guarantees anyway, and at
            // hundreds of thousands of flushes that duplication drowns
            // the UI thread.
            emitBatch(callbacks, batch);
            batchCount_.fetch_add(1, std::memory_order_relaxed);
            if (!force && (now - lastFlush >= kFlushInterval)) {
                emitChain(callbacks, chainLeaf);
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
            // Live analytics at most every second engine-wide (same
            // single-winner pattern); the exact finals come from run().
            auto lastA = lastAnalyticsMs_.load(std::memory_order_relaxed);
            if (ms - lastA >= 1000
                && lastAnalyticsMs_.compare_exchange_weak(lastA, ms, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
                emitAnalytics(callbacks);
            }
            // Throttle transitions ride the same periodic slot: exactly one
            // worker announces each change for status display.
            const bool throttledNow = throttlePolicy_.throttled();
            bool announced = lastThrottleEmitted_.load(std::memory_order_relaxed);
            if (throttledNow != announced
                && lastThrottleEmitted_.compare_exchange_strong(announced, throttledNow,
                                                               std::memory_order_relaxed,
                                                               std::memory_order_relaxed)
                && callbacks.onThrottleChanged) {
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onThrottleChanged(throttledNow);
            }
        }
    };

    DirJob job{};
    while (true) {
        waitForPermit(index);
        if (!queue_.pop(job)) {
            break; // cancelled: no more work will ever arrive
        }
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
            node.ownerId = entry.ownerId;

            if (entry.isDirectory && !entry.isReparsePoint) {
                auto childAcc = std::make_unique<DirAcc>();
                childAcc->parent = job.acc;
                childAcc->id = node.id;
                DirAcc* childAccPtr = childAcc.get();

                for (DirAcc* up = job.acc; up != nullptr; up = up->parent) {
                    localAgg[up].folders += 1;
                }

                localDirs.push_back(std::move(childAcc));

                batch.push_back(std::move(node));
                ++localDirsN;
                queue_.push(DirJob{entry.fullPath, childAccPtr, entry.backendToken});
            } else {
                for (DirAcc* up = job.acc; up != nullptr; up = up->parent) {
                    LocalAgg& agg = localAgg[up];
                    agg.logical += entry.logicalSize;
                    agg.physical += entry.physicalSize;
                    agg.files += 1;
                }

                const bool ageKnown = node.lastWriteTs != 0;
                const std::uint64_t ageDays =
                    (ageKnown && runStartUnix_ >= node.lastWriteTs)
                        ? (runStartUnix_ - node.lastWriteTs) / 86400ULL
                        : 0;
                analytics_.recordFileLazy(
                    node.id, node.nameUtf8, entry.logicalSize, ageDays, ageKnown, entry.ownerId,
                    [&] { return utf8Path(entry.fullPath); });

                batch.push_back(std::move(node));
                ++localFiles;
                localBytes += entry.logicalSize;
            }
            if (callbacks.onProgress
                && progressTick_.fetch_add(1, std::memory_order_relaxed) % 2000 == 0) {
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onProgress(utf8Name(entry.fullPath));
            }
            maybeFlush(job.acc, false);
        }
        finder->close();
        queue_.taskDone();
    }
    maybeFlush(nullptr, true);
    mergeAgg(); // thread exit (incl. cancel): no counted node is left behind

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
    lastAnalyticsMs_.store(0, std::memory_order_relaxed);
    userCancelled_.store(false, std::memory_order_relaxed);
    analytics_.reset();
    throttlePolicy_.reset();
    lastThrottleEmitted_.store(false, std::memory_order_relaxed);
    setWorkerCap(kMaxWorkers);
    scanId_ = scanId; // read-only for the rest of the run (workers spawn below)

    auto startTime = std::chrono::steady_clock::now();
    runStart_ = startTime;
    runStartUnix_ = static_cast<std::uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
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
    std::uint64_t loadMs{0};
    if (backend_ == BackendKind::DirectMft) {
        const auto probeStart = std::chrono::steady_clock::now();
        MftFinder probe{};
        if (!probe.open(root)) {
            backend_ = BackendKind::NativeDirectory;
            if (callbacks.onNotice) {
                // Downgrade, not a skip: carry the probe's reason so the
                // log (and any future elevation troubleshooting) names it.
                std::string detail = probe.lastError();
                if (!detail.empty()) {
                    detail = " (" + detail + ")";
                }
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onNotice("Direct volume reading unavailable" + detail
                                   + "; using native enumeration.");
            }
        }
        loadMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                                  - probeStart)
                .count());
    }

    unsigned workers = std::thread::hardware_concurrency();
    if (workers < 2U) {
        workers = 2U;
    }
    workers = (std::min)(workers, kMaxWorkers);

    queue_.push(DirJob{root, rootAcc, 0});
    std::vector<std::thread> pool{};
    for (unsigned i{0}; i < workers; ++i) {
        pool.emplace_back([this, &callbacks, i] { workerMain(callbacks, i); });
    }
    queue_.waitUntilIdle();
    queue_.requestCancel();
    for (auto& t : pool) {
        t.join();
    }
    const auto joinEnd = std::chrono::steady_clock::now();
    emitSweep(callbacks);
    emitAnalytics(callbacks); // exact finals now that workers joined
    const std::uint64_t sweepMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                              - joinEnd)
            .count());
    if (callbacks.onThrottleChanged
        && throttlePolicy_.throttled() != lastThrottleEmitted_.load(std::memory_order_relaxed)) {
        // A transition with no later flush to announce it (quiet tail).
        lastThrottleEmitted_.store(throttlePolicy_.throttled(), std::memory_order_relaxed);
        callbacks.onThrottleChanged(throttlePolicy_.throttled());
    }
    auto endTime = std::chrono::steady_clock::now();

    EngineResult result{};
    result.fileCount = fileCount_.load(std::memory_order_relaxed);
    result.dirCount = dirCount_.load(std::memory_order_relaxed);
    result.totalBytes = totalBytes_.load(std::memory_order_relaxed);
    result.backend = backend_;
    result.cancelled = userCancelled_.load(std::memory_order_relaxed);
    result.durationMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());
    result.batchCount = batchCount_.load(std::memory_order_relaxed);
    result.errorCount = errorCount_.load(std::memory_order_relaxed);
    result.loadMs = loadMs;
    result.sweepMs = sweepMs;
    loadMs_ = loadMs;
    sweepMs_ = sweepMs;
    emitTelemetry(callbacks, "final");
    return result;
}

} // namespace usa::scanner
