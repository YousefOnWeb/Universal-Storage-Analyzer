#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

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
};

QTEST_MAIN(TestLocalScanner)
#include "test_scanner.moc"
