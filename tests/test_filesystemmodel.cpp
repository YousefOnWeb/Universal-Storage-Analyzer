#include <QtTest>

#include <QDateTime>
#include <QDir>

#include <cmath>

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
        QCOMPARE(model.data(dSize, FileSystemModel::Roles::SortRole).toULongLong(), static_cast<qulonglong>(500));
        QCOMPARE(model.data(dFiles).toUInt(), static_cast<quint32>(1));

        // Root keeps exact totals — 600, not 1200.
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(600));
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // the visible volume root
        const QModelIndex rootIdx = model.indexForNodeId(0);
        QVERIFY(rootIdx.isValid());
        QCOMPARE(model.rowCount(rootIdx), 2); // D + the "[1 Files]" group
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
        // Root-level files aggregate (spec 005c), so root shows D plus
        // the "[1 Files]" group — never 3 rows.
        QCOMPARE(model.rowCount(QModelIndex{}), 1); // the visible root, never 3
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 2); // D + group beneath root
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
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColSize, dIdx.parent()),
                            FileSystemModel::Roles::SortRole)
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
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColSize, dIdx.parent()),
                            FileSystemModel::Roles::SortRole)
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
        QCOMPARE(model.rowCount(model.indexForNodeId(0)), 1); // the group row beneath it
        const QModelIndex groupIdx = model.indexForNodeId(FileSystemModel::kRootFilesGroupId);
        QVERIFY(groupIdx.isValid());
        QCOMPARE(model.rowCount(groupIdx), 1); // f beneath the group
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
        // Spec 006a: a file row IS one file and zero folders — real
        // values in both roles, never an empty "uncomputed" cell.
        QCOMPARE(model.data(filesCell).toUInt(), static_cast<quint32>(1));
        QCOMPARE(model.data(filesCell, FileSystemModel::Roles::SortRole).toUInt(),
                 static_cast<quint32>(1));
        const QModelIndex foldersCell =
            model.index(fIdx.row(), FileSystemModel::ColFolders, fIdx.parent());
        QCOMPARE(model.data(foldersCell).toUInt(), static_cast<quint32>(0));
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
        // Root-level files aggregate (spec 005c): second child is the
        // group row, and f lives beneath it with its size intact.
        QCOMPARE(kids[1].id, FileSystemModel::kRootFilesGroupId);
        const auto members = model.childEntries(FileSystemModel::kRootFilesGroupId);
        QCOMPARE(members.size(), static_cast<std::size_t>(1));
        QCOMPARE(members[0].id, static_cast<quint32>(2));
        QCOMPARE(members[0].logicalSize, static_cast<std::uint64_t>(100));
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
        QCOMPARE(model.rowCount(rootIdx), 2); // D + the "[1 Files]" group (f1 aggregates)

        // Children address back to the root row, not to invalid.
        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());
        QCOMPARE(model.parent(dIdx), rootIdx);

        // Root selection projects its children into details.
        DetailsTableModel details{&model};
        details.setSelectedNode(rootIdx);
        QCOMPARE(details.rowCount(), 2);
    }

    void groupsRootLevelFilesWithExactSums()
    {
        // Spec 005c: root-level files aggregate into one "[N Files]"
        // group row (no individual tree rows); sums reconcile exactly;
        // members list through the group; later batches grow it.
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
        f1.name = QStringLiteral("a.sys");
        f1.logicalSize = 100;
        f1.allocatedSize = 4096;
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 0;
        f2.name = QStringLiteral("b.sys");
        f2.logicalSize = 200;
        f2.allocatedSize = 8192;
        FileRecord g1{};
        g1.id = 4;
        g1.parentId = 1;
        g1.name = QStringLiteral("g");
        g1.logicalSize = 50;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1, f2, g1});

        const QModelIndex rootIdx = model.indexForNodeId(0);
        QVERIFY(rootIdx.isValid());
        QCOMPARE(model.rowCount(rootIdx), 2); // D + group (never 3)
        const QModelIndex groupIdx = model.indexForNodeId(FileSystemModel::kRootFilesGroupId);
        QVERIFY(groupIdx.isValid());
        QCOMPARE(model.parent(groupIdx), rootIdx);
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColName, rootIdx)).toString(),
                 QStringLiteral("[2 Files]"));
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColSize, rootIdx),
                            FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(300));
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColAllocated, rootIdx),
                            FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(12288));
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColFiles, rootIdx)).toUInt(),
                 static_cast<quint32>(2));
        // Totals count members once (group sums are display-only).
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(350));

        // Members list through the group, individually addressable.
        QCOMPARE(model.rowCount(groupIdx), 2);
        QVERIFY(model.indexForNodeId(2).isValid());
        QVERIFY(model.indexForNodeId(3).isValid());
        QCOMPARE(model.parent(model.indexForNodeId(2)), groupIdx);

        // A later batch grows the same group row in place.
        FileRecord f3{};
        f3.id = 5;
        f3.parentId = 0;
        f3.name = QStringLiteral("c.sys");
        f3.logicalSize = 50;
        f3.allocatedSize = 4096;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{f3});
        QCOMPARE(model.rowCount(rootIdx), 2); // still D + group
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColName, rootIdx)).toString(),
                 QStringLiteral("[3 Files]"));
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColSize, rootIdx),
                            FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(350));
        QCOMPARE(model.rowCount(groupIdx), 3);
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(400));
    }

    void omitsGroupRowWithoutRootFiles()
    {
        // No root-level files: directories list directly, no group row.
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir});

        const QModelIndex rootIdx = model.indexForNodeId(0);
        QCOMPARE(model.rowCount(rootIdx), 1); // D only
        QVERIFY(!model.indexForNodeId(FileSystemModel::kRootFilesGroupId).isValid());
    }

    void formatsSizesPerUnitWithRawSortKeys()
    {
        // Spec 006b: display strings honor the unit selector (Auto
        // buckets by magnitude); SortRole stays raw; tooltips carry
        // exact bytes; switching re-renders via layoutChanged.
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord a{};
        a.id = 2;
        a.parentId = 1;
        a.name = QStringLiteral("a");
        a.logicalSize = 512;
        a.allocatedSize = 4096;
        FileRecord b{};
        b.id = 3;
        b.parentId = 1;
        b.name = QStringLiteral("b");
        b.logicalSize = 5368709120ULL; // 5 GiB
        b.allocatedSize = 5368709120ULL;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, a, b});

        const QModelIndex dIdx = model.indexForNodeId(1);
        const QModelIndex aSize = model.index(0, FileSystemModel::ColSize, dIdx);
        const QModelIndex bSize = model.index(1, FileSystemModel::ColSize, dIdx);
        // Default unit is Auto: bytes stay bytes, gibibytes scale up.
        QCOMPARE(model.data(aSize).toString(), QStringLiteral("512.00 B"));
        QCOMPARE(model.data(bSize).toString(), QStringLiteral("5.00 GB"));
        QCOMPARE(model.data(aSize, FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(512));
        QCOMPARE(model.data(aSize, Qt::ToolTipRole).toString(),
                 QStringLiteral("512 bytes"));
        QCOMPARE(model.data(bSize, Qt::ToolTipRole).toString(),
                 QStringLiteral("5368709120 bytes"));

        QSignalSpy layoutSpy{&model, &FileSystemModel::layoutChanged};
        model.setSizeUnit(QStringLiteral("MB"));
        QCOMPARE(layoutSpy.count(), 1);
        QCOMPARE(model.data(aSize).toString(), QStringLiteral("0.00 MB"));
        QCOMPARE(model.data(bSize).toString(), QStringLiteral("5120.00 MB"));
        QCOMPARE(model.data(aSize, FileSystemModel::Roles::SortRole).toULongLong(),
                 static_cast<qulonglong>(512));
        model.setSizeUnit(QStringLiteral("KB"));
        QCOMPARE(model.data(bSize).toString(), QStringLiteral("5242880.00 KB"));
        model.setSizeUnit(QStringLiteral("KB")); // no-op: no second signal
        QCOMPARE(layoutSpy.count(), 2);
    }

    void shareOfParentComputesExactly()
    {
        // Spec 006a: per-child share of the selected parent's logical
        // total (one decimal); root and zero denominators show the em
        // dash and sort below real shares.
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

        const QModelIndex rootIdx = model.indexForNodeId(0);
        // No summaries yet: parent total is 0, every share undefined.
        QCOMPARE(model.data(model.index(0, FileSystemModel::ColSharePct, rootIdx)).toString(),
                 QStringLiteral("—"));
        const QModelIndex groupIdx = model.indexForNodeId(FileSystemModel::kRootFilesGroupId);
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColSharePct, rootIdx),
                            FileSystemModel::Roles::SortRole).toDouble(), -1.0);

        BranchMetrics r{};
        r.nodeId = 0;
        r.logicalSize = 600;
        r.fileCount = 1;
        r.folderCount = 1;
        BranchMetrics d{};
        d.nodeId = 1;
        d.logicalSize = 500;
        d.fileCount = 1;
        model.onBranchMetricsUpdated(QStringLiteral("t"), r);
        model.onBranchMetricsUpdated(QStringLiteral("t"), d);

        // D holds 500/600, the group (f1: 100) holds 100/600, root is dash.
        const QModelIndex dIdx = model.indexForNodeId(1);
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColSharePct, rootIdx)).toString(),
                 QStringLiteral("83.3%"));
        QVERIFY(std::abs(model.data(model.index(dIdx.row(), FileSystemModel::ColSharePct, rootIdx),
                                    FileSystemModel::Roles::SortRole).toDouble()
                         - 100.0 * 500.0 / 600.0)
                < 1e-9);
        QCOMPARE(model.data(model.index(groupIdx.row(), FileSystemModel::ColSharePct, rootIdx)).toString(),
                 QStringLiteral("16.7%"));
        QCOMPARE(model.data(model.index(0, FileSystemModel::ColSharePct, QModelIndex{})).toString(),
                 QStringLiteral("—"));
        QCOMPARE(model.headerData(FileSystemModel::ColSharePct, Qt::Horizontal).toString(),
                 QStringLiteral("% of Parent"));
    }

    void ownerColumnResolvesWithUnknownHonesty()
    {
        // Spec 006c: owner ids flow record -> node -> resolved display;
        // empty ids render the honest "(unknown)" (with an explanatory
        // tooltip); unresolvable ids pass through verbatim on every
        // platform (mirrors the resolver unit-test precedent).
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        FileRecord f{};
        f.id = 2;
        f.parentId = 0; // root-level: aggregates into the group
        f.name = QStringLiteral("f");
        f.logicalSize = 50;
        f.ownerId = "S-1-5-9999-8888-7777"; // bogus everywhere: verbatim
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f});

        const QModelIndex rootIdx = model.indexForNodeId(0);
        const QModelIndex dIdx = model.indexForNodeId(1);
        const QModelIndex fIdx = model.indexForNodeId(2);
        const QModelIndex groupIdx = model.indexForNodeId(FileSystemModel::kRootFilesGroupId);
        QVERIFY(groupIdx.isValid());
        QCOMPARE(model.parent(fIdx), groupIdx); // member, not a tree row

        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColOwner, rootIdx)).toString(),
                 QStringLiteral("(unknown)"));
        QCOMPARE(model.data(model.index(dIdx.row(), FileSystemModel::ColOwner, rootIdx),
                            FileSystemModel::Roles::SortRole).toString(),
                 QStringLiteral("(unknown)"));
        QVERIFY(!model.data(model.index(dIdx.row(), FileSystemModel::ColOwner, rootIdx),
                            Qt::ToolTipRole).toString().isEmpty());
        QCOMPARE(model.data(model.index(fIdx.row(), FileSystemModel::ColOwner, groupIdx)).toString(),
                 QStringLiteral("S-1-5-9999-8888-7777"));
        QCOMPARE(model.data(model.index(0, FileSystemModel::ColOwner, QModelIndex{})).toString(),
                 QStringLiteral("(unknown)")); // root has no owner either
        QCOMPARE(model.headerData(FileSystemModel::ColOwner, Qt::Horizontal).toString(),
                 QStringLiteral("Owner"));
        QCOMPARE(model.headerData(FileSystemModel::ColAccessed, Qt::Horizontal).toString(),
                 QStringLiteral("Last Accessed"));
        // Unknown access times stay empty (never a guessed date).
        QVERIFY(!model.data(model.index(fIdx.row(), FileSystemModel::ColAccessed, groupIdx)).isValid());
    }

    void accessColumnShowsBackendTime()
    {
        // Spec 006d: backend access times render like modification
        // times; zero stays empty; SortRole stays raw.
        FileSystemModel model{};
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
        f.lastModifiedTs = 1700000000;
        f.lastAccessTs = 1700086400;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f});

        const QModelIndex fIdx = model.indexForNodeId(2);
        QVERIFY(fIdx.isValid());
        QCOMPARE(model.data(model.index(fIdx.row(), FileSystemModel::ColAccessed, fIdx.parent())).toString(),
                 QDateTime::fromSecsSinceEpoch(static_cast<qint64>(1700086400))
                     .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        QCOMPARE(model.data(model.index(fIdx.row(), FileSystemModel::ColAccessed, fIdx.parent()),
                            FileSystemModel::Roles::SortRole).toLongLong(),
                 static_cast<qlonglong>(1700086400));
    }

    void directoryRecencyTakesSubtreeMax()
    {
        // Spec 006e: directory recency is max(object mtime, subtree-max
        // from summaries); a stale summary never rewinds a newer value;
        // files keep their own mtime (no summaries address them).
        FileSystemModel model{};
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord dir{};
        dir.id = 1;
        dir.parentId = 0;
        dir.name = QStringLiteral("D");
        dir.isDirectory = true;
        dir.lastModifiedTs = 1000; // stale directory object
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir});

        const QModelIndex dIdx = model.indexForNodeId(1);
        QVERIFY(dIdx.isValid());
        const QModelIndex cell = model.index(dIdx.row(), FileSystemModel::ColModified, dIdx.parent());

        BranchMetrics fresh{};
        fresh.nodeId = 1;
        fresh.maxMtime = 2000; // newer content beneath
        model.onBranchMetricsUpdated(QStringLiteral("t"), fresh);
        QCOMPARE(model.data(cell, FileSystemModel::Roles::SortRole).toLongLong(),
                 static_cast<qlonglong>(2000));

        BranchMetrics stale{};
        stale.nodeId = 1;
        stale.maxMtime = 500; // older report: must not rewind
        model.onBranchMetricsUpdated(QStringLiteral("t"), stale);
        QCOMPARE(model.data(cell, FileSystemModel::Roles::SortRole).toLongLong(),
                 static_cast<qlonglong>(2000));
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
