#include "ScannerService.h"

ScannerService::ScannerService(QObject* parent)
    : IScannerService(parent)
    , thread_(new QThread(this))
    , worker_(new LocalScannerWorker())
{
    worker_->moveToThread(thread_);
    connect(this, &ScannerService::startRequested, worker_, &LocalScannerWorker::startScanPath);
    connect(this, &ScannerService::pauseRequested, worker_, &LocalScannerWorker::requestPause);
    connect(this, &ScannerService::cancelRequested, worker_, &LocalScannerWorker::requestCancel);

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

QString ScannerService::startScan(const QString& targetPath)
{
    activeTaskId_ = QStringLiteral("scan-%1").arg(++taskCounter_);
    emit startRequested(activeTaskId_, targetPath);
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
