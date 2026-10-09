#include "ScannerService.h"

ScannerService::ScannerService(QObject* parent)
    : IScannerService(parent)
    , thread_(new QThread(this))
    , worker_(new LocalScannerWorker())
    , telemetryThread_(new QThread(this))
    , telemetry_(new DiskTelemetry())
{
    worker_->moveToThread(thread_);
    telemetry_->moveToThread(telemetryThread_);
    connect(this, &ScannerService::startRequested, worker_, &LocalScannerWorker::startScanPath);
    // Pause/cancel bypass the worker event loop on purpose: the engine only
    // touches thread-safe primitives, so a direct connection takes effect
    // immediately even while a scan blocks the worker thread.
    connect(this, &ScannerService::pauseRequested, worker_, &LocalScannerWorker::requestPause,
            Qt::DirectConnection);
    connect(this, &ScannerService::cancelRequested, worker_, &LocalScannerWorker::requestCancel,
            Qt::DirectConnection);

    // Worker re-emits cross a thread boundary (worker thread -> GUI
    // thread). Queued explicitly so delivery stays queued even if the
    // affinity of either endpoint ever changes.
    connect(worker_, &LocalScannerWorker::scanStarted, this, &ScannerService::scanStarted,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::nodesDiscovered, this, &ScannerService::nodesDiscovered,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::branchMetricsUpdated, this,
            &ScannerService::branchMetricsUpdated, Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::scanProgress, this, &ScannerService::scanProgress,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::scanFinished, this, &ScannerService::scanFinished,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::errorOccurred, this, &ScannerService::errorOccurred,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::noticeOccurred, this, &ScannerService::noticeOccurred,
            Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::analyticsUpdated, this,
            &ScannerService::analyticsUpdated, Qt::QueuedConnection);
    connect(worker_, &LocalScannerWorker::throttleChanged, this,
            &ScannerService::throttleChanged, Qt::QueuedConnection);
    // Sampler runs on its own thread; samples apply straight into the
    // engine, which only touches atomics plus the permit condition
    // (same DirectConnection contract as pause/cancel).
    connect(telemetry_, &DiskTelemetry::sampleReady, worker_,
            &LocalScannerWorker::applyThrottleSample, Qt::DirectConnection);
    connect(this, &ScannerService::telemetryStartRequested, telemetry_,
            &DiskTelemetry::startMonitoring);
    connect(this, &ScannerService::scanFinished, telemetry_, &DiskTelemetry::stopMonitoring);

    thread_->start();
    telemetryThread_->start();
}

ScannerService::~ScannerService()
{
    // The worker thread may be blocked inside ScanEngine::run(), which
    // quit() alone never interrupts. Cancel first so the engine unwinds
    // (requestCancel also wakes a paused worker), then route deletion
    // through the worker's own event loop before quitting: a deleteLater()
    // posted after wait() would never be delivered.
    worker_->requestCancel();
    worker_->deleteLater();
    thread_->quit();
    if (!thread_->wait(5000)) {
        thread_->terminate();
        thread_->wait();
    }
    // The sampler owns no blocking work: queue a stop, delete through its
    // loop, then quit. Queued posts precede quit, so both are delivered.
    QMetaObject::invokeMethod(telemetry_, &DiskTelemetry::stopMonitoring,
                              Qt::QueuedConnection);
    telemetry_->deleteLater();
    telemetryThread_->quit();
    telemetryThread_->wait();
}

QString ScannerService::startScan(const QString& targetPath, bool enableLogging)
{
    activeTaskId_ = QStringLiteral("scan-%1").arg(++taskCounter_);
    emit telemetryStartRequested(targetPath);
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
