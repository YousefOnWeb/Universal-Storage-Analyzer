#pragma once

#include "IScannerService.h"
#include "ScanTypes.h"

#include <QVector>
#include <atomic>

// Worker QObject moved to a dedicated QThread via moveToThread().
// Never touches widgets; communicates only through queued signals/slots.
class LocalScannerWorker : public QObject {
    Q_OBJECT
public:
    static constexpr int kBatchSize = 500;
    static constexpr quint64 kClusterSize = 4096;

    explicit LocalScannerWorker(QObject* parent = nullptr);

public slots:
    void startScanPath(const QString& taskId, const QString& targetPath);
    void requestPause(bool paused);
    void requestCancel();

signals:
    void scanStarted(const QString& taskId, const QString& targetPath);
    void nodesDiscovered(const QString& taskId, QVector<FileRecord> batch);
    void branchMetricsUpdated(const QString& taskId, BranchMetrics metrics);
    void scanProgress(const QString& taskId, const QString& currentPath);
    void scanFinished(const QString& taskId, quint64 fileCount, quint64 dirCount,
                      quint64 totalBytes);
    void errorOccurred(const QString& taskId, const QString& message);

private:
    std::atomic_bool paused_{false};
    std::atomic_bool cancelled_{false};
};
