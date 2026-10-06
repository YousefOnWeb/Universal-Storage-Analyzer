#include "ScanEngine.h"

#include <algorithm>
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

void ScanEngine::workerMain(const EngineCallbacks& callbacks)
{
    auto finder = makeFinder();
    std::vector<EngineNode> batch{};
    batch.reserve(kBatchNodes);
    auto lastFlush = std::chrono::steady_clock::now();
    const auto maybeFlush = [&](DirAcc* chainLeaf, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!batch.empty()
            && (force || batch.size() >= kBatchNodes || now - lastFlush >= kFlushInterval)) {
            emitBatch(callbacks, batch);
            emitChain(callbacks, chainLeaf);
            lastFlush = now;
        }
    };

    DirJob job{};
    while (queue_.pop(job)) {
        if (!finder->open(job.path)) {
            if (callbacks.onError) {
                std::lock_guard<std::mutex> lock{emitMutex_};
                callbacks.onError("Cannot enumerate directory: " + utf8Name(job.path));
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
                
                {
                    std::lock_guard<std::mutex> lock{dirsMutex_};
                    allDirs_.push_back(std::move(childAcc));
                }

                batch.push_back(std::move(node));
                dirCount_.fetch_add(1, std::memory_order_relaxed);
                queue_.push(DirJob{entry.fullPath, childAccPtr});
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
}

EngineResult ScanEngine::run(const std::filesystem::path& root, const EngineCallbacks& callbacks)
{
    nextId_.store(1, std::memory_order_relaxed);
    fileCount_.store(0, std::memory_order_relaxed);
    dirCount_.store(0, std::memory_order_relaxed);
    totalBytes_.store(0, std::memory_order_relaxed);
    progressTick_.store(0, std::memory_order_relaxed);
    userCancelled_.store(false, std::memory_order_relaxed);
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
            callbacks.onError("Target path does not exist: " + utf8Name(root));
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
                callbacks.onError("Direct volume reading unavailable; using native enumeration.");
            }
        }
    }

    unsigned workers = std::thread::hardware_concurrency();
    if (workers < 2U) {
        workers = 2U;
    }
    workers = (std::min)(workers, kMaxWorkers);

    queue_.push(DirJob{root, rootAcc});
    std::vector<std::thread> pool{};
    for (unsigned i{0}; i < workers; ++i) {
        pool.emplace_back([this, &callbacks] { workerMain(callbacks); });
    }
    queue_.waitUntilIdle();
    queue_.requestCancel();
    for (auto& t : pool) {
        t.join();
    }
    EngineResult result{};
    result.fileCount = fileCount_.load(std::memory_order_relaxed);
    result.dirCount = dirCount_.load(std::memory_order_relaxed);
    result.totalBytes = totalBytes_.load(std::memory_order_relaxed);
    result.backend = backend_;
    result.cancelled = userCancelled_.load(std::memory_order_relaxed);
    return result;
}

} // namespace usa::scanner
