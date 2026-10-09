#include <QtTest>

#include <QApplication>
#include <QLabel>
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
#include "../src/ui/MainWindow.h"
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
        auto* details = win.findChild<QTableView*>();
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
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    TestUi test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_ui.moc"
