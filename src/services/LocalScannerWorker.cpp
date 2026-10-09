#include "LocalScannerWorker.h"

#include <QDebug>

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

#ifdef _WIN32
    const std::filesystem::path root{targetPath.toStdWString()};
#else
    const std::filesystem::path root{targetPath.toStdString()};
#endif

    // Re-chunk engine batches (128/timeout-driven) into UI batches (500).
    // Callbacks fire on engine worker threads; pending_ is shared.
    std::vector<FileRecord> pending{};
    pending.reserve(static_cast<std::size_t>(kBatchSize));

    usa::scanner::EngineCallbacks callbacks{};
    callbacks.onBatch = [&](std::vector<EngineNode> batch) {
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
    scanActive_.store(false, std::memory_order_release);
    emit scanFinished(taskId, result.fileCount, result.dirCount, result.totalBytes);
}
