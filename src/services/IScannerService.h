#pragma once

#include <QObject>
#include <QString>

// Contract: IScannerService (see docs/architecture/ipc-contracts.md).
// Sprint 1 implements the single-active-scan subset; multi-task IDs are
// reserved so later milestones can run concurrent drive/cloud scans.
class IScannerService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString startScan(const QString& targetPath, bool enableLogging = true) = 0;
    virtual void toggleScanState(const QString& taskId, bool pause) = 0;
    virtual void cancelScan(const QString& taskId) = 0;

signals:
    void scanProgress(const QString& taskId, const QString& currentPath);
};
