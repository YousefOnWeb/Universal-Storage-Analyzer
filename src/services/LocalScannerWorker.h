#pragma once

#include <QObject>
#include <QVector>

#include <atomic>
#include <mutex>

#include "scanner/ScanEngine.h"
#include "DiskTelemetry.h"
#include "OwnerResolver.h"
#include "ScanTypes.h"

// Thin Qt bridge over ScanEngine: translates backend-neutral engine output
// into FileRecord batches and Qt signals. The engine does all traversal,
// threading, batching, and aggregation; this class owns no scan results.
//
// Threading: startScanPath() blocks the worker thread inside
// ScanEngine::run(). requestPause()/requestCancel() only touch the
// engine's thread-safe primitives, so the service invokes them with a
// direct connection and they take effect even mid-scan. The engine is
// single-flight: overlapping startScanPath() calls are rejected with a
// non-fatal error.
//
// Threading: startScanPath() blocks the worker thread inside
// ScanEngine::run(). requestPause()/requestCancel() only touch the
// engine's thread-safe primitives, so the service invokes them with a
// direct connection and they take effect even mid-scan.
class LocalScannerWorker : public QObject {
    Q_OBJECT
public:
    static constexpr int kBatchSize = 500;

    explicit LocalScannerWorker(QObject* parent = nullptr);

public slots:
    void startScanPath(const QString& taskId, const QString& targetPath, bool enableLogging = true);
    void requestPause(bool paused);
    void requestCancel();
    // Throttle samples apply straight into the engine (atomics plus the
    // permit condition only): DirectConnection-safe like pause/cancel.
    void applyThrottleSample(const usa::DiskSample& sample);

signals:
    void scanStarted(const QString& taskId, const QString& targetPath);
    void nodesDiscovered(const QString& taskId, QVector<FileRecord> batch);
    void branchMetricsUpdated(const QString& taskId, BranchMetrics metrics);
    void scanProgress(const QString& taskId, const QString& currentPath);
    void scanFinished(const QString& taskId, quint64 fileCount, quint64 dirCount,
                      quint64 totalBytes);
    void errorOccurred(const QString& taskId, const QString& message, bool fatal);
    // Informational notices (backend downgrades): shown transiently, never
    // collected as skipped items.
    void noticeOccurred(const QString& taskId, const QString& message);
    void analyticsUpdated(const QString& taskId, const AnalyticsData& data);
    void throttleChanged(const QString& taskId, bool throttled);

private:
    usa::scanner::ScanEngine engine_;
    OwnerResolver resolver_;
    std::mutex pendingMutex_;
    std::atomic<bool> scanActive_{false};
};
