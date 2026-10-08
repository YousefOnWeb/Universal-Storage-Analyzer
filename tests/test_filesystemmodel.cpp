#include <QtTest>

#include "../src/models/FileSystemModel.h"
#include "../src/services/ScanTypes.h"

// Regression test for the aggregation contract: the engine owns all
// aggregation and streams absolute per-directory totals, so the model must
// store nodes and summaries verbatim. Adding either side would double
// directory sizes and counts.
class TestFileSystemModel : public QObject {
    Q_OBJECT
private slots:
    void storesNodesAndSummariesWithoutDoubling()
    {        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));

        // dir D (id 1) + file f1 (id 2) under root; file f2 (id 3) under D.
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord f1{};
        f1.id = 2;
        f1.parentId = 0;
        f1.name = QStringLiteral("f1");
        f1.logicalSize = 100;
        f1.allocatedSize = 4096;
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 1;
        f2.name = QStringLiteral("f2");
        f2.logicalSize = 500;
        f2.allocatedSize = 4096;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1, f2});

        // Pre-summary: raw nodes stored, no totals fabricated.
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(600));

        BranchMetrics d{};
        d.nodeId = 1;
        d.logicalSize = 500;
        d.allocatedSize = 4096;
        d.fileCount = 1;
        d.folderCount = 0;
        BranchMetrics r{};
        r.nodeId = 0;
        r.logicalSize = 600;
        r.allocatedSize = 8192;
        r.fileCount = 1;
        r.folderCount = 1;
        model.onBranchMetricsUpdated(QStringLiteral("t"), d);
        model.onBranchMetricsUpdated(QStringLiteral("t"), r);

        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());
        // D's own row values (not its child's): size 500, one file.
        const QModelIndex dSize =
            model.index(dIdx.row(), FileSystemModel::ColSize, dIdx.parent());
        const QModelIndex dFiles =
            model.index(dIdx.row(), FileSystemModel::ColFiles, dIdx.parent());
        QCOMPARE(model.data(dSize).toULongLong(), static_cast<qulonglong>(500));
        QCOMPARE(model.data(dFiles).toUInt(), static_cast<quint32>(1));

        // Root keeps exact totals — 600, not 1200.
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(600));
        QCOMPARE(model.rowCount(QModelIndex{}), 2); // D + f1
        QCOMPARE(model.rowCount(dIdx), 1); // f2
    }

    void coalescesRapidSummarySignals()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));

        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir});
        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());

        // Fifty synchronous summaries: values apply immediately, but the
        // view notification fires exactly once via the coalescing timer.
        QSignalSpy dataSpy{&model, &FileSystemModel::dataChanged};
        for (int i{0}; i < 50; ++i) {
            BranchMetrics m{};
            m.nodeId = 1;
            m.logicalSize = static_cast<quint64>(100 + i);
            model.onBranchMetricsUpdated(QStringLiteral("t"), m);
        }
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColSize, dIdx.parent()))
                     .toULongLong(),
                 static_cast<qulonglong>(149));
        QVERIFY(dataSpy.wait(500));
        QCOMPARE(dataSpy.count(), 1);
    }
};

QTEST_MAIN(TestFileSystemModel)
#include "test_filesystemmodel.moc"
