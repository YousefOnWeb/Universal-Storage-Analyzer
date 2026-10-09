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
