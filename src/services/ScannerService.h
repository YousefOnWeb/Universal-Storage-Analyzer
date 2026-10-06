#pragma once

#include "IScannerService.h"
#include "LocalScannerWorker.h"

#include <QThread>

// GUI-thread façade: owns the worker thread, exposes IScannerService.
// Follows the moveToThread() pattern — never subclass QThread for logic.
class ScannerService final : public IScannerService {
    Q_OBJECT
public:
    explicit ScannerService(QObject* parent = nullptr);
    ~ScannerService() override;

    ScannerService(const ScannerService&) = delete;
    ScannerService& operator=(const ScannerService&) = delete;

    QString startScan(const QString& targetPath, bool enableLogging = true) override;
    void toggleScanState(const QString& taskId, bool pause) override;
    void cancelScan(const QString& taskId) override;

signals:
    void startRequested(const QString& taskId, const QString& targetPath, bool enableLogging);
    void pauseRequested(bool paused);
    void cancelRequested();
    // Re-emitted worker events for UI consumption:
    void scanStarted(const QString& taskId, const QString& targetPath);
    void nodesDiscovered(const QString& taskId, QVector<FileRecord> batch);
    void branchMetricsUpdated(const QString& taskId, BranchMetrics metrics);
    void scanFinished(const QString& taskId, quint64 fileCount, quint64 dirCount,
                      quint64 totalBytes);
    void errorOccurred(const QString& taskId, const QString& message);

private:
    QThread* thread_{nullptr};
    LocalScannerWorker* worker_{nullptr};
    QString activeTaskId_{};
    int taskCounter_{0};
};
