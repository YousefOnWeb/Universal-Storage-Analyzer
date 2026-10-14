#include <QtTest>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <fstream>
#include <string>

#include "../src/models/FileSystemModel.h"
#include "../src/services/ScannerService.h"

// End-to-end pipeline: real ScannerService + FileSystemModel wired exactly
// like src/app/main.cpp, scanning a temp tree. Proves engine output flows
// through signals into exact on-screen totals.
class TestPipeline : public QObject {
    Q_OBJECT
private slots:
    void scanReachesModelWithExactTotals()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        {
            std::ofstream f{root / "a.bin", std::ios::binary};
            f.write(std::string(100, 'x').data(), 100);
        }
        std::filesystem::create_directory(root / "sub");
        {
            std::ofstream f{root / "sub" / "b.bin", std::ios::binary};
            f.write(std::string(200, 'y').data(), 200);
        }

        ScannerService svc{};
        FileSystemModel model{};
        QObject::connect(&svc, &ScannerService::nodesDiscovered, &model,
                         &FileSystemModel::onNodesDiscovered);
        QObject::connect(&svc, &ScannerService::branchMetricsUpdated, &model,
                         &FileSystemModel::onBranchMetricsUpdated);
        QObject::connect(&svc, &ScannerService::sweepUpdated, &model,
                         &FileSystemModel::onSweepUpdated);
        QObject::connect(&svc, &ScannerService::scanStarted, &model,
                         &FileSystemModel::onScanStarted);

        QSignalSpy doneSpy{&svc, &ScannerService::scanFinished};
        const QString task = svc.startScan(tmp.path());
        QVERIFY(!task.isEmpty());
        QVERIFY(doneSpy.wait(15000));
        QCOMPARE(doneSpy.count(), 1);

        // scanFinished is emitted after all node/summary signals from the
        // worker thread, so queued delivery has fully landed by now.
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(300));
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // the visible volume root
        const QModelIndex rootIdx = model.indexForNodeId(0);
        QVERIFY(rootIdx.isValid());

        // Locate "sub" and check its exact subtree total (200, not doubled).
        // Root-level files aggregate (spec 005c): a.bin lives in the group.
        quint64 subBytes{0};
        int subRows{-1};
        quint64 groupBytes{0};
        QCOMPARE(model.rowCount(rootIdx), 2); // sub + group beneath root
        for (int r{0}; r < model.rowCount(rootIdx); ++r) {
            const QModelIndex nameIdx = model.index(r, FileSystemModel::ColName, rootIdx);
            if (model.data(nameIdx).toString() == QStringLiteral("sub")) {
                subBytes = model.data(model.index(r, FileSystemModel::ColSize, rootIdx))
                               .toULongLong();
                subRows = model.rowCount(model.index(r, 0, rootIdx));
            }
            if (model.nodeIdForIndex(model.index(r, 0, rootIdx))
                == FileSystemModel::kRootFilesGroupId) {
                groupBytes = model.data(model.index(r, FileSystemModel::ColSize, rootIdx))
                                 .toULongLong();
            }
        }
        QCOMPARE(subBytes, static_cast<qulonglong>(200));
        QCOMPARE(subRows, 1); // b.bin
        QCOMPARE(groupBytes, static_cast<qulonglong>(100)); // a.bin via the group
    }

    void destroysServiceWhileScanSuspended()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        {
            std::ofstream f{root / "a.bin", std::ios::binary};
            f.write(std::string(100, 'x').data(), 100);
        }

        auto* svc = new ScannerService();
        QSignalSpy startedSpy{svc, &ScannerService::scanStarted};
        QSignalSpy doneSpy{svc, &ScannerService::scanFinished};
        const QString task = svc->startScan(tmp.path());
        // Direct connection: suspends the engine before the worker thread
        // picks up the queued start, so the scan is guaranteed in flight.
        svc->toggleScanState(task, true);
        QTRY_VERIFY(startedSpy.count() == 1);
        QVERIFY(doneSpy.isEmpty()); // suspended: cannot have finished

        // Must cancel the suspended engine and return promptly instead of
        // blocking forever or leaking the worker.
        QElapsedTimer timer{};
        timer.start();
        delete svc;
        QVERIFY(timer.elapsed() < 30000);
    }
};

QTEST_MAIN(TestPipeline)
#include "test_pipeline.moc"
