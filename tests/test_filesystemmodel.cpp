#include <QtTest>

#include <QDir>

#include "../src/models/DetailsTableModel.h"
#include "../src/models/FileSystemModel.h"
#include "../src/models/StatsTableModel.h"
#include "../src/models/TreeFilterProxy.h"
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
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // the visible volume root
        const QModelIndex rootIdx = model.indexForNodeId(0);
        QVERIFY(rootIdx.isValid());
        QCOMPARE(model.rowCount(rootIdx), 2); // D + f1 beneath root
        QCOMPARE(model.rowCount(dIdx), 1); // f2
    }

    void defersChildArrivingBeforeItsParent()
    {
        // Workers race: a child batch can land before the parent's batch.
        // The child must wait (not misattach to root) and land correctly.
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));

        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 1; // D does not exist yet
        f2.name = QStringLiteral("f2");
        f2.logicalSize = 500;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{f2});
        // Nothing stored yet — certainly not at root: only the visible
        // root row itself, with no children and no bytes.
        QCOMPARE(model.rowCount(QModelIndex{}), 1);
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 0);
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(0));

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
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1});

        // Parent arrival drains the waiter into place; totals stay exact.
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // the visible root, never 3
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 2); // D + f1 beneath root
        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());
        QCOMPARE(model.rowCount(dIdx), 1); // f2
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(600));
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

    void appliesSummaryArrivingBeforeItsNode()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));

        // Summaries stream immediately while node batches are chunked, so a
        // summary may precede its node. Latest wins; applied on insert.
        BranchMetrics early{};
        early.nodeId = 7;
        early.logicalSize = 900;
        early.allocatedSize = 8192;
        early.fileCount = 2;
        model.onBranchMetricsUpdated(QStringLiteral("t"), early);

        FileRecord dir{};
        dir.id = 7;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir});

        const QModelIndex dIdx = model.indexForNodeId(7);
        QVERIFY(dIdx.isValid());
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColSize, dIdx.parent()))
                     .toULongLong(),
                 static_cast<qulonglong>(900));
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColFiles, dIdx.parent()))
                     .toUInt(),
                 static_cast<quint32>(2));
    }

    void resetsStateOnNewScan()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t1"), QStringLiteral("C:/a"));
        FileRecord f{};
        f.id = 2;
        f.parentId = 0;
        f.name = QStringLiteral("f");
        f.logicalSize = 100;
        model.onNodesDiscovered(QStringLiteral("t1"), QVector<FileRecord>{f});
        QVERIFY(model.rowCount(QModelIndex{}) > 0);

        QSignalSpy resetSpy{&model, &FileSystemModel::modelResetForNewScan};
        model.onScanStarted(QStringLiteral("t2"), QStringLiteral("D:/b"));
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(resetSpy.at(0).at(0).toString(), QStringLiteral("D:/b"));
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // fresh visible root, no children
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 0);
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(0));
        QVERIFY(!model.indexForNodeId(2).isValid()); // old ids are gone

        model.onNodesDiscovered(QStringLiteral("t2"), QVector<FileRecord>{f});
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // still just the root row
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 1); // f beneath it
        model.clear();
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // clear rebuilds the "/" root
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(0));
    }

    void pinsIndexRolesAndHeaders()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord f{};
        f.id = 2;
        f.parentId = 0;
        f.name = QStringLiteral("f");
        f.logicalSize = 100;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{f});

        // The volume root is a visible, addressable row (spec 005a);
        // unknown ids still have no index.
        const QModelIndex rootIdx = model.indexForNodeId(0);
        QVERIFY(rootIdx.isValid());
        QCOMPARE(model.data(model.index(rootIdx.row(), FileSystemModel::ColName)).toString(),
                 QDir::toNativeSeparators(QStringLiteral("C:/data")));
        QVERIFY(!model.parent(rootIdx).isValid()); // root is top-level
        QVERIFY(!model.indexForNodeId(4242).isValid());

        QCOMPARE(model.headerData(FileSystemModel::ColName, Qt::Horizontal).toString(),
                 QStringLiteral("Name"));
        QCOMPARE(model.headerData(FileSystemModel::ColSize, Qt::Horizontal).toString(),
                 QStringLiteral("Size"));
        QVERIFY(!model.headerData(-1, Qt::Horizontal).isValid());
        QVERIFY(!model.headerData(0, Qt::Vertical).isValid());

        const QModelIndex fIdx = model.indexForNodeId(2);
        QVERIFY(fIdx.isValid());
        const QModelIndex filesCell =
            model.index(fIdx.row(), FileSystemModel::ColFiles, fIdx.parent());
        // Files carry no counts: DisplayRole stays empty while SortRole
        // pins a numeric zero so proxies sort instead of guessing.
        QVERIFY(!model.data(filesCell).isValid());
        QCOMPARE(model.data(filesCell, FileSystemModel::Roles::SortRole).toUInt(),
                 static_cast<quint32>(0));
        QCOMPARE(model.nodeIdForIndex(fIdx), static_cast<quint32>(2));
        QCOMPARE(model.nodeIdForIndex(QModelIndex{}), static_cast<quint32>(0));
    }

    void detailsSurvivesSourceReset()
    {
        FileSystemModel model{};
        DetailsTableModel details{&model};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));

        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord f{};
        f.id = 2;
        f.parentId = 1;
        f.name = QStringLiteral("f");
        f.logicalSize = 50;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f});

        details.setSelectedNode(model.indexForNodeId(1));
        QCOMPARE(details.rowCount(), 1);

        // A new scan resets the source: the stored selection must invalidate
        // cleanly (empty details, no dangling reads) instead of following
        // freed nodes.
        model.onScanStarted(QStringLiteral("t2"), QStringLiteral("D:/b"));
        QCOMPARE(details.rowCount(), 0);
        QVERIFY(!details.data(details.index(0, 0)).isValid());
    }

    void exposesChildrenForLayout()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord f{};
        f.id = 2;
        f.parentId = 0;
        f.name = QStringLiteral("f");
        f.logicalSize = 100;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f});

        QVERIFY(model.isDirectoryNode(1));
        QVERIFY(!model.isDirectoryNode(2));
        QVERIFY(model.isDirectoryNode(0)); // root anchor
        QVERIFY(!model.isDirectoryNode(4242));

        const auto kids = model.childEntries(0);
        QCOMPARE(kids.size(), static_cast<std::size_t>(2));
        QCOMPARE(kids[0].id, static_cast<quint32>(1));
        QCOMPARE(kids[0].name, QStringLiteral("D"));
        QVERIFY(kids[0].isDirectory);
        QCOMPARE(kids[1].logicalSize, static_cast<quint64>(100));
        QVERIFY(model.childEntries(4242).empty());
        QVERIFY(model.childEntries(2).empty()); // files have no children
    }

    void exposesVisibleRootRowWithTargetName()
    {
        // Spec 005a: the scan root is a visible top-level row named for
        // the target; children hang beneath it; selecting it shows them
        // in the details projection.
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
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
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1});

        QCOMPARE(model.rowCount(QModelIndex{}), 1);
        const QModelIndex rootIdx = model.index(0, FileSystemModel::ColName, QModelIndex{});
        QVERIFY(rootIdx.isValid());
        QCOMPARE(model.nodeIdForIndex(rootIdx), static_cast<quint32>(0));
        QCOMPARE(model.data(rootIdx).toString(),
                 QDir::toNativeSeparators(QStringLiteral("C:/data")));
        QCOMPARE(model.indexForNodeId(0), rootIdx);
        QVERIFY(!model.parent(rootIdx).isValid());
        QCOMPARE(model.rowCount(rootIdx), 2); // D + f1

        // Children address back to the root row, not to invalid.
        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());
        QCOMPARE(model.parent(dIdx), rootIdx);

        // Root selection projects its children into details.
        DetailsTableModel details{&model};
        details.setSelectedNode(rootIdx);
        QCOMPARE(details.rowCount(), 2);
    }

    void treeFilterSearchesRecursively()
    {
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord f1{};
        f1.id = 2;
        f1.parentId = 0;
        f1.name = QStringLiteral("f1.txt");
        f1.logicalSize = 100;
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 1;
        f2.name = QStringLiteral("f2.log");
        f2.logicalSize = 500;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1, f2});

        TreeFilterProxyModel proxy{};
        proxy.setSourceModel(&model);
        QCOMPARE(proxy.rowCount(), 1); // unfiltered: the visible root

        // Search narrows to matches, keeping ancestor chains visible.
        proxy.setSearchText(QStringLiteral("f2"));
        QCOMPARE(proxy.rowCount(), 1); // root (keeping D keeping f2.log)
        const QModelIndex rootProxy = proxy.index(0, 0);
        QCOMPARE(proxy.rowCount(rootProxy), 1); // D beneath root
        const QModelIndex dProxy = proxy.index(0, 0, rootProxy);
        QCOMPARE(proxy.rowCount(dProxy), 1);
        QCOMPARE(proxy.data(proxy.index(0, 0, dProxy)).toString(), QStringLiteral("f2.log"));

        // No match hides everything; clearing restores the root.
        proxy.setSearchText(QStringLiteral("nope"));
        QCOMPARE(proxy.rowCount(), 0);
        proxy.setSearchText(QStringLiteral(""));
        QCOMPARE(proxy.rowCount(), 1);
    }

    void statsModelReplacesRowsWithSortKeys()
    {
        StatsTableModel model{};
        model.setHeaders({QStringLiteral("Extension"), QStringLiteral("Bytes")});
        QCOMPARE(model.columnCount(), 2);
        QCOMPARE(model.headerData(0, Qt::Horizontal).toString(), QStringLiteral("Extension"));

        StatsRow a{};
        a.text = {QStringLiteral(".log"), QStringLiteral("300 (50.0%)")};
        a.keys = {0, 300};
        StatsRow b{};
        b.text = {QStringLiteral(".mp4"), QStringLiteral("300 (50.0%)")};
        b.keys = {0, 300};
        b.nodeId = 9;
        model.setRows({a, b});
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("300 (50.0%)"));
        QCOMPARE(model.data(model.index(0, 1), FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(300));
        QCOMPARE(model.rowNodeId(1), static_cast<quint32>(9));
        QCOMPARE(model.rowNodeId(7), static_cast<quint32>(0));

        model.setRows({});
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_MAIN(TestFileSystemModel)
#include "test_filesystemmodel.moc"
