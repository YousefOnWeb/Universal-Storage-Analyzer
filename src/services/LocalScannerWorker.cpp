#include "LocalScannerWorker.h"

#include <QDebug>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

#include "ScanLogger.h"

using usa::scanner::EngineDirSummary;
using usa::scanner::EngineNode;

LocalScannerWorker::LocalScannerWorker(QObject* parent)
    : QObject(parent)
{
}

void LocalScannerWorker::requestPause(bool paused)
{
    engine_.setPaused(paused);
}

void LocalScannerWorker::requestCancel()
{
    engine_.requestCancel();
}

void LocalScannerWorker::applyThrottleSample(const usa::DiskSample& sample)
{
    usa::scanner::ThrottleInput in{};
    in.available = sample.available;
    in.latencyMs = sample.latencyMs;
    in.queueDepth = sample.queueDepth;
    in.activePct = sample.activePct;
    engine_.applyThrottleSample(in);
    // Raw sample log (spec 004 hardening): the policy verdict alone
    // cannot tell disk-busy from CPU-side stalls after the fact. One
    // line/sec during scans only (the sampler runs scan-bounded); the
    // task id is mutex-guarded because samples arrive on the telemetry
    // thread while scans start/finish on this thread.
    QString task{};
    {
        std::lock_guard<std::mutex> lock{pendingMutex_};
        task = activeTaskId_;
    }
    if (sample.available) {
        qInfo().noquote() << QStringLiteral("{\"scan_id\":\"%1\",\"disk\":{\"available\":true,"
                                             "\"latencyMs\":%2,\"queueDepth\":%3,\"activePct\":%4}}")
                                 .arg(task.isEmpty() ? QStringLiteral("none") : task)
                                 .arg(sample.latencyMs, 0, 'f', 1)
                                 .arg(sample.queueDepth, 0, 'f', 2)
                                 .arg(sample.activePct, 0, 'f', 1);
    } else {
        qInfo().noquote() << QStringLiteral("{\"scan_id\":\"%1\",\"disk\":{\"available\":false}}")
                                 .arg(task.isEmpty() ? QStringLiteral("none") : task);
    }
}

void LocalScannerWorker::applyOptions(const QString& ageBucketsCsv, int topN,
                                      bool throttleEnabled)
{
    // Lenient parse: the dialog validates, this keeps garbage out.
    std::vector<std::uint64_t> edges{};
    for (const QString& part : ageBucketsCsv.split(QStringLiteral(","), Qt::SkipEmptyParts)) {
        bool ok{false};
        const std::uint64_t days = part.trimmed().toULongLong(&ok);
        if (ok && days > 0 && (edges.empty() || days > edges.back())) {
            edges.push_back(days);
        }
    }
    if (!edges.empty()) {
        engine_.setAgeBuckets(std::move(edges));
    }
    if (topN > 0) {
        engine_.setTopN(static_cast<std::size_t>(topN));
    }
    engine_.setThrottleEnabled(throttleEnabled);
}

void LocalScannerWorker::startScanPath(const QString& taskId, const QString& targetPath, bool enableLogging)
{
    // The engine is single-flight: a second run would share its queue and
    // accumulators. Reject overlaps with a non-fatal error instead.
    if (scanActive_.exchange(true, std::memory_order_acq_rel)) {
        emit errorOccurred(taskId, QStringLiteral("Scan already in progress; ignoring request."),
                           false);
        return;
    }

    emit scanStarted(taskId, targetPath);
    {
        std::lock_guard<std::mutex> lock{pendingMutex_};
        activeTaskId_ = taskId;
    }

#ifdef _WIN32
    const std::filesystem::path root{targetPath.toStdWString()};
#else
    const std::filesystem::path root{targetPath.toStdString()};
#endif

    // Re-chunk engine batches into UI batches (500). The engine streams
    // in MFT/traversal order, so parents scatter: sorting each batch by
    // parentId first turns hundreds of one-row model transactions into a
    // handful of contiguous inserts downstream. Callbacks fire on engine
    // worker threads; pending_ is shared.
    std::vector<FileRecord> pending{};
    pending.reserve(static_cast<std::size_t>(kBatchSize));

    usa::scanner::EngineCallbacks callbacks{};
    callbacks.onBatch = [&](std::vector<EngineNode> batch) {
        std::stable_sort(batch.begin(), batch.end(), [](const EngineNode& a, const EngineNode& b) {
            return a.parentId < b.parentId;
        });
        QVector<FileRecord> ready{};
        {
            std::lock_guard<std::mutex> lock{pendingMutex_};
            for (auto& node : batch) {
                FileRecord rec{};
                rec.id = node.id;
                rec.parentId = node.parentId;
                rec.name = QString::fromStdString(node.nameUtf8);
                rec.logicalSize = node.logicalSize;
                rec.allocatedSize = node.physicalSize;
                rec.lastModifiedTs = node.lastWriteTs;
                rec.isDirectory = node.isDirectory;
                rec.ownerId = node.ownerId;
                pending.push_back(std::move(rec));
            }
            if (pending.size() >= static_cast<std::size_t>(kBatchSize)) {
                ready = QVector<FileRecord>(pending.begin(), pending.end());
                pending.clear();
            }
        }
        // Emit outside the lock: slots must never serialize on the
        // engine's accumulation mutex.
        if (!ready.isEmpty()) {
            emit nodesDiscovered(taskId, ready);
        }
    };
    callbacks.onDirSummary = [&](const EngineDirSummary& summary) {
        BranchMetrics metrics{};
        metrics.nodeId = summary.nodeId;
        metrics.logicalSize = summary.logicalSize;
        metrics.allocatedSize = summary.physicalSize;
        metrics.fileCount = summary.fileCount;
        metrics.folderCount = summary.folderCount;
        emit branchMetricsUpdated(taskId, metrics);
    };
    callbacks.onError = [&](const std::string& message, bool fatal) {
        emit errorOccurred(taskId, QString::fromStdString(message), fatal);
    };
    callbacks.onProgress = [&](const std::string& path) {
        emit scanProgress(taskId, QString::fromStdString(path));
    };
    callbacks.onTelemetry = [&](const std::string& json) {
        qInfo().noquote() << QString::fromStdString(json);
    };
    callbacks.onThrottleChanged = [&](bool throttled) {
        // Transitions are rare (hysteresis) and load-bearing for speed
        // diagnosis: always log, then deliver for status display.
        qInfo().noquote() << QStringLiteral("{\"scan_id\":\"%1\",\"throttle\":%2}")
                                 .arg(taskId, throttled ? "true" : "false");
        emit throttleChanged(taskId, throttled);
    };
    callbacks.onNotice = [&](const std::string& message) {
        // Session-logged like telemetry: downgrades must leave a trail
        // even when no UI/CLI consumer is attached (e.g. test runs).
        qInfo().noquote() << QString::fromStdString(message);
        emit noticeOccurred(taskId, QString::fromStdString(message));
    };
    callbacks.onAnalytics = [&](const usa::scanner::AnalyticsSnapshot& snap) {
        // Engine streams snapshots throttled (live ≤1 Hz + exact finals);
        // forward verbatim after Qt projection.
        AnalyticsData data{};
        data.totalBytes = snap.totalBytes;
        data.totalFiles = snap.totalFiles;
        const double total = static_cast<double>(snap.totalBytes);
        for (const auto& [ext, st] : snap.extensions) {
            AnalyticsExtension row{};
            row.extension = QString::fromStdString(ext);
            row.bytes = st.bytes;
            row.files = st.files;
            row.sharePct = total > 0.0 ? (static_cast<double>(st.bytes) / total * 100.0)
                                       : 0.0;
            data.extensions.push_back(std::move(row));
        }
        for (const auto& entry : snap.top) {
            AnalyticsTopFile row{};
            row.nodeId = entry.id;
            row.path = QString::fromStdString(entry.pathUtf8);
            row.name = QString::fromStdString(entry.nameUtf8);
            row.bytes = entry.bytes;
            data.top.push_back(std::move(row));
        }
        for (std::size_t i{0}; i < snap.ageBytes.size(); ++i) {
            AnalyticsAgeBucket row{};
            row.label = ageBucketLabel(i, snap.ageEdges);
            row.files = (i < snap.ageFiles.size()) ? snap.ageFiles[i] : 0;
            row.bytes = snap.ageBytes[i];
            data.age.push_back(std::move(row));
        }
        for (const auto& [owner, st] : snap.owners) {
            AnalyticsOwner row{};
            row.owner = resolver_.displayName(owner);
            row.bytes = st.bytes;
            row.files = st.files;
            row.sharePct = total > 0.0 ? (static_cast<double>(st.bytes) / total * 100.0)
                                       : 0.0;
            data.owners.push_back(std::move(row));
        }
        emit analyticsUpdated(taskId, data);
    };
    callbacks.onSweep = [&](std::vector<EngineDirSummary> summaries) {
        // Bulk finals ride as one queued event; the model applies values
        // immediately and lets its coalescer pace the repaint.
        QVector<BranchMetrics> out{};
        out.reserve(static_cast<int>(summaries.size()));
        for (const auto& s : summaries) {
            BranchMetrics m{};
            m.nodeId = s.nodeId;
            m.logicalSize = s.logicalSize;
            m.allocatedSize = s.physicalSize;
            m.fileCount = s.fileCount;
            m.folderCount = s.folderCount;
            out.push_back(std::move(m));
        }
        emit sweepUpdated(taskId, out);
    };

    const usa::scanner::EngineResult result =
        engine_.run(root, callbacks, taskId.toUtf8().toStdString());

    QVector<FileRecord> tail{};
    {
        std::lock_guard<std::mutex> lock{pendingMutex_};
        if (!pending.empty()) {
            tail = QVector<FileRecord>(pending.begin(), pending.end());
            pending.clear();
        }
    }
    if (!tail.isEmpty()) {
        emit nodesDiscovered(taskId, tail);
    }
    if (enableLogging) {
        ScanLogger::writeLog(taskId, targetPath, result);
    }
    {
        std::lock_guard<std::mutex> lock{pendingMutex_};
        activeTaskId_.clear();
    }
    scanActive_.store(false, std::memory_order_release);
    emit scanFinished(taskId, result.fileCount, result.dirCount, result.totalBytes);
}
