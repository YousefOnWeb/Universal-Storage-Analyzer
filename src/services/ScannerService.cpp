#include "ScannerService.h"

ScannerService::ScannerService(QObject* parent)
    : IScannerService(parent)
    , thread_(new QThread(this))
    , worker_(new LocalScannerWorker())
{
    worker_->moveToThread(thread_);
    connect(this, &ScannerService::startRequested, worker_, &LocalScannerWorker::startScanPath);
    // Pause/cancel bypass the worker event loop on purpose: the engine only
    // touches thread-safe primitives, so a direct connection takes effect
    // immediately even while a scan blocks the worker thread.
    connect(this, &ScannerService::pauseRequested, worker_, &LocalScannerWorker::requestPause,
            Qt::DirectConnection);
    connect(this, &ScannerService::cancelRequested, worker_, &LocalScannerWorker::requestCancel,
            Qt::DirectConnection);

    connect(worker_, &LocalScannerWorker::scanStarted, this, &ScannerService::scanStarted);
    connect(worker_, &LocalScannerWorker::nodesDiscovered, this, &ScannerService::nodesDiscovered);
    connect(worker_, &LocalScannerWorker::branchMetricsUpdated, this,
            &ScannerService::branchMetricsUpdated);
    connect(worker_, &LocalScannerWorker::scanProgress, this, &ScannerService::scanProgress);
    connect(worker_, &LocalScannerWorker::scanFinished, this, &ScannerService::scanFinished);
    connect(worker_, &LocalScannerWorker::errorOccurred, this, &ScannerService::errorOccurred);

    thread_->start();
}

ScannerService::~ScannerService()
{
    thread_->quit();
    thread_->wait();
    worker_->deleteLater();
}

QString ScannerService::startScan(const QString& targetPath, bool enableLogging)
{
    activeTaskId_ = QStringLiteral("scan-%1").arg(++taskCounter_);
    emit startRequested(activeTaskId_, targetPath, enableLogging);
    return activeTaskId_;
}

void ScannerService::toggleScanState(const QString& taskId, bool pause)
{
    if (taskId != activeTaskId_) {
        return;
    }
    emit pauseRequested(pause);
}

void ScannerService::cancelScan(const QString& taskId)
{
    if (taskId != activeTaskId_) {
        return;
    }
    emit cancelRequested();
}
