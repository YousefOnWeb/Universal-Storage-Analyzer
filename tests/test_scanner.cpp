#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include <thread>

#include "../src/services/LocalScannerWorker.h"

class TestLocalScanner : public QObject {
    Q_OBJECT
private slots:
    void scansTemporaryDirectory()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        QFile f1{tmp.filePath(QStringLiteral("a.txt"))};
        QVERIFY(f1.open(QIODevice::WriteOnly));
        f1.write(QByteArray(1024, 'x'));
        f1.close();
        QDir().mkdir(tmp.filePath(QStringLiteral("sub")));
        QFile f2{tmp.filePath(QStringLiteral("sub/b.bin"))};
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(QByteArray(2048, 'y'));
        f2.close();

        LocalScannerWorker worker{};
        QSignalSpy batchSpy{&worker, &LocalScannerWorker::nodesDiscovered};
        QSignalSpy doneSpy{&worker, &LocalScannerWorker::scanFinished};
        QSignalSpy errSpy{&worker, &LocalScannerWorker::errorOccurred};

        worker.startScanPath(QStringLiteral("t1"), tmp.path());

        if (!errSpy.isEmpty()) {
            for (const auto& args : errSpy) {
                qInfo() << "scan error:" << args.at(1).toString();
            }
        }
        QVERIFY(errSpy.isEmpty());
        QCOMPARE(doneSpy.count(), 1);
        QVERIFY(!batchSpy.isEmpty());

        qlonglong totalRecords{0};
        for (const auto& args : batchSpy) {
            totalRecords += static_cast<qlonglong>(
                args.at(1).value<QVector<FileRecord>>().size());
        }
        // a.txt + sub + sub/b.bin (the root anchor comes from scanStarted,
        // not from node batches)
        QCOMPARE(totalRecords, static_cast<qlonglong>(3));

        const auto done = doneSpy.at(0);
        QCOMPARE(done.at(1).toULongLong(), static_cast<qulonglong>(2)); // files
        QCOMPARE(done.at(2).toULongLong(), static_cast<qulonglong>(1)); // dirs
        QCOMPARE(done.at(3).toULongLong(), static_cast<qulonglong>(3072)); // bytes
    }

    void reportsMissingPath()
    {
        LocalScannerWorker worker{};
        QSignalSpy errSpy{&worker, &LocalScannerWorker::errorOccurred};
        QSignalSpy doneSpy{&worker, &LocalScannerWorker::scanFinished};
        worker.startScanPath(QStringLiteral("t2"),
                             QStringLiteral("Z:/definitely-not-here-usa-test"));
        QCOMPARE(errSpy.count(), 1);
        QCOMPARE(doneSpy.count(), 1);
    }

    void forwardsAnalyticsSnapshot()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        QFile f1{tmp.filePath(QStringLiteral("a.log"))};
        QVERIFY(f1.open(QIODevice::WriteOnly));
        f1.write(QByteArray(100, 'x'));
        f1.close();
        QFile f2{tmp.filePath(QStringLiteral("b.bin"))};
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(QByteArray(200, 'y'));
        f2.close();

        LocalScannerWorker worker{};
        QSignalSpy analyticsSpy{&worker, &LocalScannerWorker::analyticsUpdated};
        QSignalSpy doneSpy{&worker, &LocalScannerWorker::scanFinished};
        worker.startScanPath(QStringLiteral("t3"), tmp.path());
        QCOMPARE(doneSpy.count(), 1);
        QVERIFY(!analyticsSpy.isEmpty()); // exact finals always fire

        const AnalyticsData data =
            analyticsSpy.at(analyticsSpy.count() - 1).at(1).value<AnalyticsData>();
        QCOMPARE(data.totalBytes, static_cast<quint64>(300));
        QCOMPARE(data.totalFiles, static_cast<quint64>(2));
        QCOMPARE(data.top.size(), 2);
        QCOMPARE(data.top[0].bytes, static_cast<quint64>(200));
        QVERIFY(data.top[0].nodeId != 0);
        QVERIFY(!data.extensions.isEmpty());
        QVERIFY(!data.age.isEmpty());
        QVERIFY(!data.owners.isEmpty());
        std::uint64_t extBytes{0};
        for (const auto& row : data.extensions) {
            extBytes += row.bytes;
        }
        QCOMPARE(extBytes, static_cast<quint64>(300)); // extensions cover every byte
    }

    void rejectsOverlappingScan()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        QFile f{tmp.filePath(QStringLiteral("a.txt"))};
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1024, 'x'));
        f.close();

        LocalScannerWorker worker{};
        QSignalSpy startedSpy{&worker, &LocalScannerWorker::scanStarted};
        QSignalSpy doneSpy{&worker, &LocalScannerWorker::scanFinished};
        QSignalSpy errSpy{&worker, &LocalScannerWorker::errorOccurred};

        // Pre-pause: the engine starts suspended, so the first scan is
        // guaranteed in flight when the second one arrives.
        worker.requestPause(true);
        std::thread first{[&] { worker.startScanPath(QStringLiteral("t1"), tmp.path()); }};
        QTRY_VERIFY(startedSpy.count() == 1);

        // Runs on this thread while the first scan is suspended: rejected
        // with a non-fatal error and never emits scanStarted.
        worker.startScanPath(QStringLiteral("t2"), tmp.path());

        worker.requestPause(false);
        first.join();

        QCOMPARE(startedSpy.count(), 1);
        QCOMPARE(doneSpy.count(), 1);
        QCOMPARE(errSpy.count(), 1);
        QCOMPARE(errSpy.at(0).at(0).toString(), QStringLiteral("t2"));
        QVERIFY(!errSpy.at(0).at(2).toBool());
    }
};

QTEST_MAIN(TestLocalScanner)
#include "test_scanner.moc"
