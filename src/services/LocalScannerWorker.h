#pragma once

#include <QObject>
#include <QVector>

#include <mutex>

#include "scanner/ScanEngine.h"
#include "ScanTypes.h"

// Thin Qt bridge over ScanEngine: translates backend-neutral engine output
// into FileRecord batches and Qt signals. The engine does all traversal,
// threading, batching, and aggregation; this class owns no scan state.
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

signals:
    void scanStarted(const QString& taskId, const QString& targetPath);
    void nodesDiscovered(const QString& taskId, QVector<FileRecord> batch);
    void branchMetricsUpdated(const QString& taskId, BranchMetrics metrics);
    void scanProgress(const QString& taskId, const QString& currentPath);
    void scanFinished(const QString& taskId, quint64 fileCount, quint64 dirCount,
                      quint64 totalBytes);
    void errorOccurred(const QString& taskId, const QString& message, bool fatal);

private:
    usa::scanner::ScanEngine engine_;
    std::mutex pendingMutex_;
};
