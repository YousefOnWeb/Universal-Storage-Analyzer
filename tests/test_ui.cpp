#include <QtTest>

#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QMap>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>

#include <fstream>
#include <string>

#include "../src/models/FileSystemModel.h"
#include "../src/services/ScannerService.h"
#include "../src/ui/BarWidget.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/OptionsDialog.h"
#include "../src/ui/PieWidget.h"
#include "../src/ui/ThemeManager.h"
#include "../src/ui/TreemapWidget.h"

// MainWindow behavior under the offscreen platform (CTest label `ui`,
// QT_QPA_PLATFORM=offscreen): model content reaches the views, and the
// error-severity contract holds end to end (fatal pops, non-fatal
// collects into one end-of-scan summary).
namespace {

QApplication* testApp()
{
    return qobject_cast<QApplication*>(QCoreApplication::instance());
}

void wireLikeMain(FileSystemModel& model, ScannerService& scanner)
{
    // Same connections as runGui() in src/app/main.cpp.
    QObject::connect(&scanner, &ScannerService::nodesDiscovered, &model,
                     &FileSystemModel::onNodesDiscovered, Qt::QueuedConnection);
    QObject::connect(&scanner, &ScannerService::branchMetricsUpdated, &model,
                     &FileSystemModel::onBranchMetricsUpdated, Qt::QueuedConnection);
    QObject::connect(&scanner, &ScannerService::scanStarted, &model,
                     &FileSystemModel::onScanStarted, Qt::QueuedConnection);
}

QLineEdit* targetEdit(QWidget* win)
{
    for (auto* edit : win->findChildren<QLineEdit*>()) {
        if (edit->placeholderText().contains(QStringLiteral("folder"))) {
            return edit;
        }
    }
    return nullptr;
}

QPushButton* startButton(QWidget* win)
{
    for (auto* btn : win->findChildren<QPushButton*>()) {
        if (btn->text() == QStringLiteral("Start")) {
            return btn;
        }
    }
    return nullptr;
}

QLabel* statusLabel(QWidget* win)
{
    for (auto* label : win->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Ready")) {
            return label;
        }
    }
    return nullptr;
}

// Prefs are ambient user state (a running app instance, earlier runs):
// back up the options/* keys, pin exactly what the test needs, restore
// on scope exit. RAII matters because QVERIFY aborts the slot, which
// would skip plain end-of-test cleanup. Construction is explicit
// org/app, exactly like production code: default-constructed QSettings
// resolves nowhere useful in test binaries (no names are set here).
struct SettingsGuard {
    QSettings settings{QStringLiteral("UniversalStorageAnalyzer"),
                       QStringLiteral("UniversalStorageAnalyzer")};
    QMap<QString, QVariant> prior{};

    explicit SettingsGuard(const QStringList& keys)
    {
        for (const auto& k : keys) {
            prior[k] = settings.value(k, QVariant{});
        }
    }

    ~SettingsGuard()
    {
        for (auto it = prior.begin(); it != prior.end(); ++it) {
            if (it.value().isValid()) {
                settings.setValue(it.key(), it.value());
            } else {
                settings.remove(it.key());
            }
        }
        settings.sync();
    }
};

} // namespace

class TestUi : public QObject {
    Q_OBJECT
private slots:
    void showsScannedContentInBothPanes()
    {
        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

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
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 1;
        f2.name = QStringLiteral("f2");
        f2.logicalSize = 500;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1, f2});

        auto* tree = win.findChild<QTreeView*>();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->model()->rowCount(), 2); // D + f1

        // The tree proxy sorts (sortingEnabled), so locate D by name
        // instead of assuming insertion order.
        QModelIndex dProxyIdx{};
        for (int r = 0; r < tree->model()->rowCount(); ++r) {
            const QModelIndex idx =
                tree->model()->index(r, FileSystemModel::ColName);
            if (tree->model()->data(idx).toString() == QStringLiteral("D")) {
                dProxyIdx = idx;
            }
        }
        QVERIFY(dProxyIdx.isValid());
        QSignalSpy curSpy{tree->selectionModel(), &QItemSelectionModel::currentChanged};
        tree->setCurrentIndex(dProxyIdx);
        QCOMPARE(curSpy.count(), 1); // MainWindow sees the selection
        auto* detailsModel = win.findChild<DetailsTableModel*>();
        QVERIFY(detailsModel != nullptr);
        QCOMPARE(detailsModel->rowCount(), 1); // D's child f2
        auto* details = win.findChild<QTableView*>(QStringLiteral("Details"));
        QVERIFY(details != nullptr);
        QCOMPARE(details->model()->rowCount(), 1); // f2 under D, via proxy
        QCOMPARE(details->model()->rowCount(), 1); // f2 under D

        // A new scan resets the source: details empties instead of
        // following freed nodes, and heals on the next selection.
        model.onScanStarted(QStringLiteral("t2"), QStringLiteral("D:/b"));
        QCOMPARE(details->model()->rowCount(), 0);
    }

    void collectsSkipsIntoEndOfScanSummary()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        {
            const std::string path = tmp.filePath(QStringLiteral("a.bin")).toStdString();
            std::ofstream f{path, std::ios::binary};
            f.write(std::string(100, 'x').data(), 100);
        }

        FileSystemModel model{};
        ScannerService scanner{};
        wireLikeMain(model, scanner);
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

        auto* target = targetEdit(&win);
        QVERIFY(target != nullptr);
        target->setText(tmp.path());
        auto* start = startButton(&win);
        QVERIFY(start != nullptr);
        QLabel* status = statusLabel(&win);
        QVERIFY(status != nullptr);

        // Non-fatal error injected while the real async scan runs: it must
        // be collected, not popped, and shown once at the end.
        QSignalSpy doneSpy{&scanner, &ScannerService::scanFinished};
        QTimer closer{};
        closer.setInterval(200);
        QObject::connect(&closer, &QTimer::timeout, [] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                modal->close();
            }
        });
        closer.start();
        start->click();
        scanner.errorOccurred(QStringLiteral("t"), QStringLiteral("skip: access denied"),
                              false);
        QVERIFY(doneSpy.wait(15000));
        closer.stop();

        QCOMPARE(status->text(), QStringLiteral("Ready (1 skipped)"));
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(100));
    }

    void treemapFollowsSelectionAndSelectsTree()
    {
        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

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
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 1;
        f2.name = QStringLiteral("f2");
        f2.logicalSize = 500;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{dir, f1, f2});
        BranchMetrics dm{};
        dm.nodeId = 1;
        dm.logicalSize = 500;
        dm.allocatedSize = 4096;
        dm.fileCount = 1;
        model.onBranchMetricsUpdated(QStringLiteral("t"), dm);

        auto* treemap = win.findChild<TreemapWidget*>();
        QVERIFY(treemap != nullptr);
        // Live model churn refreshes through a 250 ms debounce: outlive it.
        treemap->refreshSoon();
        QTest::qWait(500);
        QCOMPARE(treemap->tileCount(), static_cast<std::size_t>(2)); // D + f1

        auto* tree = win.findChild<QTreeView*>();
        QVERIFY(tree != nullptr);
        // The tree proxy sorts: locate rows by name, never by position.
        const auto proxyRowByName = [&](const QString& name) {
            for (int r = 0; r < tree->model()->rowCount(); ++r) {
                const QModelIndex idx = tree->model()->index(r, FileSystemModel::ColName);
                if (tree->model()->data(idx).toString() == name) {
                    return idx;
                }
            }
            return QModelIndex{};
        };
        const QModelIndex dIdx = proxyRowByName(QStringLiteral("D"));
        QVERIFY(dIdx.isValid());
        tree->setCurrentIndex(dIdx);
        QCOMPARE(treemap->rootId(), static_cast<quint32>(1));
        QCOMPARE(treemap->tileCount(), static_cast<std::size_t>(1)); // f2

        // Tile click drives the tree: full loop through real wiring.
        auto* treeProxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
        QVERIFY(treeProxy != nullptr);
        QSignalSpy clickedSpy{treemap, &TreemapWidget::tileClicked};
        treemap->selectTile(3);
        QCOMPARE(clickedSpy.count(), 1);
        const QModelIndex curSrc = treeProxy->mapToSource(tree->currentIndex());
        QCOMPARE(model.nodeIdForIndex(curSrc), static_cast<quint32>(3));

        // Selecting a file keeps the treemap's folder context.
        tree->setCurrentIndex(proxyRowByName(QStringLiteral("f1")));
        QCOMPARE(treemap->rootId(), static_cast<quint32>(1));
    }

    void barsShowTopChildrenAndDriveSelection()
    {
        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

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
        BranchMetrics dm{};
        dm.nodeId = 1;
        dm.logicalSize = 500;
        model.onBranchMetricsUpdated(QStringLiteral("t"), dm);

        auto* bars = win.findChild<BarWidget*>();
        QVERIFY(bars != nullptr);
        bars->refreshSoon(); // live model churn refreshes debounced
        QTest::qWait(500);
        QCOMPARE(bars->barCount(), 2); // D (500) + f1 (100), desc
        QCOMPARE(bars->rootId(), static_cast<quint32>(0));

        auto* tree = win.findChild<QTreeView*>();
        QVERIFY(tree != nullptr);
        auto* treeProxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
        QVERIFY(treeProxy != nullptr);

        // Bar click drives the tree through real wiring.
        QSignalSpy clickedSpy{bars, &BarWidget::barClicked};
        bars->selectRow(1); // f1, the smaller bar
        QCOMPARE(clickedSpy.count(), 1);
        QCOMPARE(clickedSpy.at(0).at(0).toUInt(), static_cast<quint32>(2));
        const QModelIndex curSrc = treeProxy->mapToSource(tree->currentIndex());
        QCOMPARE(model.nodeIdForIndex(curSrc), static_cast<quint32>(2));

        // Bars follow folder selection like the treemap does.
        for (int r = 0; r < tree->model()->rowCount(); ++r) {
            const QModelIndex idx = tree->model()->index(r, FileSystemModel::ColName);
            if (tree->model()->data(idx).toString() == QStringLiteral("D")) {
                tree->setCurrentIndex(idx);
            }
        }
        QCOMPARE(bars->rootId(), static_cast<quint32>(1));
        QCOMPARE(bars->barCount(), 0); // D is empty in this fixture
    }

    void pieShowsVolumeAndDrivesSelection()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        {
            const std::string path = tmp.filePath(QStringLiteral("a.bin")).toStdString();
            std::ofstream f{path, std::ios::binary};
            f.write(std::string(100, 'x').data(), 100);
        }

        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

        model.onScanStarted(QStringLiteral("t"), tmp.path());
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
        BranchMetrics dm{};
        dm.nodeId = 1;
        dm.logicalSize = 500;
        model.onBranchMetricsUpdated(QStringLiteral("t"), dm);

        auto* pie = win.findChild<PieWidget*>();
        QVERIFY(pie != nullptr);
        pie->setTargetPath(tmp.path());
        pie->refreshSoon();
        QTest::qWait(500);
        // D + f1 + free space at minimum (Other files on a real volume).
        QVERIFY(pie->sliceCount() >= 3);

        auto* tree = win.findChild<QTreeView*>();
        QVERIFY(tree != nullptr);
        auto* treeProxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
        QVERIFY(treeProxy != nullptr);

        // Node slices drive the tree; Free/Other slices select nothing.
        int dSlice{-1};
        int freeSlice{-1};
        for (int i = 0; i < pie->sliceCount(); ++i) {
            if (pie->sliceNodeId(i) == 1) {
                dSlice = i;
            } else if (pie->sliceNodeId(i) == PieWidget::kNoId) {
                freeSlice = i;
            }
        }
        QVERIFY(dSlice >= 0);
        QSignalSpy clickedSpy{pie, &PieWidget::pieClicked};
        pie->selectSlice(dSlice);
        QCOMPARE(clickedSpy.count(), 1);
        const QModelIndex curSrc = treeProxy->mapToSource(tree->currentIndex());
        QCOMPARE(model.nodeIdForIndex(curSrc), static_cast<quint32>(1));
        if (freeSlice >= 0) {
            pie->selectSlice(freeSlice);
            QCOMPARE(clickedSpy.count(), 1); // no signal for non-node slices
        }
    }

    void optionsDialogValidatesAndPersists()
    {
        SettingsGuard guard({QStringLiteral("options/ageBuckets"),
                             QStringLiteral("options/topN"),
                             QStringLiteral("options/startup"),
                             QStringLiteral("options/throttle")});
        QSettings setup{QStringLiteral("UniversalStorageAnalyzer"),
                        QStringLiteral("UniversalStorageAnalyzer")};
        setup.remove(QStringLiteral("options/topN")); // pin the default path
        setup.setValue(QStringLiteral("options/startup"), QStringLiteral("last"));
        setup.setValue(QStringLiteral("options/throttle"), true);

        QVERIFY(!OptionsDialog::validateBuckets(QStringLiteral("7,7,0"), nullptr));
        QVERIFY(!OptionsDialog::validateBuckets(QStringLiteral("abc"), nullptr));
        QVERIFY(!OptionsDialog::validateBuckets(QStringLiteral(""), nullptr));
        QVERIFY(!OptionsDialog::validateBuckets(
            QStringLiteral("1,2,3,4,5,6,7,8,9,10,11,12,13"), nullptr));
        QVERIFY(OptionsDialog::validateBuckets(QStringLiteral("7,30,365"), nullptr));

        OptionsDialog dlg{};
        dlg.setBucketsCsv(QStringLiteral("7,7,0"));
        dlg.accept();
        QCOMPARE(dlg.result(), 0); // invalid: stays open, nothing saved
        QVERIFY(OptionsDialog::loadBucketsCsv() != QStringLiteral("7,7,0"));

        dlg.setBucketsCsv(QStringLiteral("7,30,365"));
        dlg.accept();
        QCOMPARE(dlg.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(OptionsDialog::loadBucketsCsv(), QStringLiteral("7,30,365"));
        QCOMPARE(OptionsDialog::loadTopN(), OptionsDialog::kDefaultTopN);
        QCOMPARE(OptionsDialog::loadStartup(), QStringLiteral("last"));
        QVERIFY(OptionsDialog::loadThrottle());
    }

    void agePaneRebucketsFromMemory()
    {
        SettingsGuard guard({QStringLiteral("options/ageBuckets")});
        QSettings setup{QStringLiteral("UniversalStorageAnalyzer"),
                        QStringLiteral("UniversalStorageAnalyzer")};
        setup.setValue(QStringLiteral("options/ageBuckets"), QStringLiteral("7,30"));
        OptionsDialog buckets{};
        buckets.accept();
        QCOMPARE(buckets.result(), static_cast<int>(QDialog::Accepted));

        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

        const std::uint64_t now =
            static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch());
        model.onScanStarted(QStringLiteral("t"), QStringLiteral("C:/data"));
        FileRecord f1{};
        f1.id = 2;
        f1.parentId = 0;
        f1.name = QStringLiteral("recent.log");
        f1.logicalSize = 100;
        f1.lastModifiedTs = now - 2 * 86400ULL;
        FileRecord f2{};
        f2.id = 3;
        f2.parentId = 0;
        f2.name = QStringLiteral("old.log");
        f2.logicalSize = 200;
        f2.lastModifiedTs = now - 40 * 86400ULL;
        model.onNodesDiscovered(QStringLiteral("t"), QVector<FileRecord>{f1, f2});

        // No engine involved: re-bucketing walks in-memory mtimes, no I/O.
        // Two edges make three buckets: [< 7 days], [7 – 30 days], [≥ 30 days].
        QCOMPARE(model.totalLogicalBytes(), static_cast<quint64>(300));
        win.refreshAgePane();
        auto* ageView = win.findChild<QTableView*>(QStringLiteral("Age of Files"));
        QVERIFY(ageView != nullptr);
        QCOMPARE(ageView->model()->rowCount(), 3);
        QCOMPARE(ageView->model()->data(ageView->model()->index(0, 0)).toString(),
                 QStringLiteral("< 7 days"));
        QCOMPARE(ageView->model()->data(ageView->model()->index(0, 1)).toUInt(),
                 static_cast<quint32>(1));
        QCOMPARE(ageView->model()->data(ageView->model()->index(1, 0)).toString(),
                 QStringLiteral("7 days – 30 days"));
        QCOMPARE(ageView->model()->data(ageView->model()->index(1, 1)).toUInt(),
                 static_cast<quint32>(0));
        QCOMPARE(ageView->model()->data(ageView->model()->index(2, 0)).toString(),
                 QStringLiteral("≥ 30 days"));
        QCOMPARE(ageView->model()->data(ageView->model()->index(2, 1)).toUInt(),
                 static_cast<quint32>(1));
        QCOMPARE(ageView->model()->data(ageView->model()->index(2, 2)).toString(),
                 QStringLiteral("200.00 B"));
    }

    void analyticsPanesPopulateFilterAndSelect()
    {
        FileSystemModel model{};
        ScannerService scanner{};
        ThemeManager themes{testApp()};
        MainWindow win{&model, &scanner, &themes};
        win.show();

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

        // Drive the panes exactly as the worker bridge would.
        AnalyticsData data{};
        data.totalBytes = 600;
        data.totalFiles = 2;
        AnalyticsExtension logRow{};
        logRow.extension = QStringLiteral(".log");
        logRow.bytes = 500;
        logRow.files = 1;
        logRow.sharePct = 83.3;
        AnalyticsExtension txtRow{};
        txtRow.extension = QStringLiteral(".txt");
        txtRow.bytes = 100;
        txtRow.files = 1;
        txtRow.sharePct = 16.7;
        data.extensions = {logRow, txtRow};
        AnalyticsTopFile topRow{};
        topRow.nodeId = 3;
        topRow.path = QStringLiteral("C:/data/D/f2.log");
        topRow.name = QStringLiteral("f2.log");
        topRow.bytes = 500;
        data.top = {topRow};
        AnalyticsOwner ownerRow{};
        ownerRow.owner = QStringLiteral("tester");
        ownerRow.bytes = 600;
        ownerRow.files = 2;
        ownerRow.sharePct = 100.0;
        data.owners = {ownerRow};
        AnalyticsAgeBucket ageRow{};
        ageRow.label = QStringLiteral("< 1 day");
        ageRow.files = 2;
        ageRow.bytes = 600;
        data.age = {ageRow};
        scanner.analyticsUpdated(QStringLiteral("t"), data);

        auto* extView = win.findChild<QTableView*>(QStringLiteral("Extensions"));
        QVERIFY(extView != nullptr);
        QCOMPARE(extView->model()->rowCount(), 2);
        auto* topView = win.findChild<QTableView*>(QStringLiteral("Top Files"));
        QVERIFY(topView != nullptr);
        QCOMPARE(topView->model()->rowCount(), 1);
        auto* usersView = win.findChild<QTableView*>(QStringLiteral("Users"));
        QVERIFY(usersView != nullptr);
        QCOMPARE(usersView->model()->rowCount(), 1);
        // Unresolvable ids pass through verbatim on every platform.
        QCOMPARE(usersView->model()->data(usersView->model()->index(0, 0)).toString(),
                 QStringLiteral("tester"));
        auto* ageView = win.findChild<QTableView*>(QStringLiteral("Age of Files"));
        QVERIFY(ageView != nullptr);
        QCOMPARE(ageView->model()->rowCount(), 1);

        // Extension pick filters the tree (dirs stay as containers).
        auto* tree = win.findChild<QTreeView*>();
        QVERIFY(tree != nullptr);
        const auto proxyRowByName = [&](const QString& name) {
            for (int r = 0; r < tree->model()->rowCount(); ++r) {
                const QModelIndex idx = tree->model()->index(r, FileSystemModel::ColName);
                if (tree->model()->data(idx).toString() == name) {
                    return idx;
                }
            }
            return QModelIndex{};
        };
        const auto extRowByName = [&](const QString& name) {
            for (int r = 0; r < extView->model()->rowCount(); ++r) {
                const QModelIndex idx = extView->model()->index(r, 0);
                if (extView->model()->data(idx).toString() == name) {
                    return idx;
                }
            }
            return QModelIndex{};
        };
        extView->selectionModel()->select(
            extRowByName(QStringLiteral(".log")),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(tree->model()->rowCount(), 1); // D only; f1.txt hidden
        const QModelIndex dIdx = proxyRowByName(QStringLiteral("D"));
        QVERIFY(dIdx.isValid());
        QCOMPARE(tree->model()->rowCount(dIdx), 1); // f2.log
        // Picking the active extension again clears the filter.
        extView->selectionModel()->clearSelection();
        extView->selectionModel()->select(
            extRowByName(QStringLiteral(".log")),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(tree->model()->rowCount(), 2); // D + f1.txt again

        // Top click selects the node in the tree through real wiring.
        topView->clicked(topView->model()->index(0, 0));
        auto* treeProxy = qobject_cast<QSortFilterProxyModel*>(tree->model());
        QVERIFY(treeProxy != nullptr);
        const QModelIndex curSrc = treeProxy->mapToSource(tree->currentIndex());
        QCOMPARE(model.nodeIdForIndex(curSrc), static_cast<quint32>(3));
    }
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    TestUi test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_ui.moc"
