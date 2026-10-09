#include "MainWindow.h"
#include "BarWidget.h"
#include "ThemeManager.h"
#include "TreemapWidget.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QStorageInfo>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(FileSystemModel* model, ScannerService* scanner, ThemeManager* themes,
                       QWidget* parent)
    : QMainWindow(parent)
    , model_(model)
    , scanner_(scanner)
    , themes_(themes)
{
    setWindowTitle(QStringLiteral("Universal Storage Analyzer"));
    resize(1200, 750);

    // Central dual-pane layout
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* topBar = new QHBoxLayout();
    targetEdit_ = new QLineEdit(central);
    targetEdit_->setPlaceholderText(QStringLiteral("Select a folder or drive to scan…"));
    targetEdit_->setText(QDir::homePath());
    targetEdit_->setAccessibleName(QStringLiteral("Scan target path"));
    auto* browseBtn = new QPushButton(QStringLiteral("Browse…"), central);
    startStopBtn_ = new QPushButton(QStringLiteral("Start"), central);
    startStopBtn_->setDefault(true);
    unitsCombo_ = new QComboBox(central);
    unitsCombo_->addItems({QStringLiteral("Auto"), QStringLiteral("KB"),
                           QStringLiteral("MB"), QStringLiteral("GB")});
    unitsCombo_->setAccessibleName(QStringLiteral("Size units"));
    quickSearch_ = new QLineEdit(central);
    quickSearch_->setPlaceholderText(QStringLiteral("Quick Search"));
    quickSearch_->setClearButtonEnabled(true);
    quickSearch_->setAccessibleName(QStringLiteral("Quick search filter"));
    topBar->addWidget(targetEdit_, 1);
    topBar->addWidget(browseBtn);
    topBar->addWidget(startStopBtn_);
    topBar->addWidget(unitsCombo_);
    topBar->addWidget(quickSearch_);
    layout->addLayout(topBar);

    splitter_ = new QSplitter(Qt::Horizontal, central);
    tree_ = new QTreeView(splitter_);
    tree_->setAccessibleName(QStringLiteral("Directory tree"));
    tree_->setUniformRowHeights(true);
    tree_->setSortingEnabled(true);

    treeProxy_ = new QSortFilterProxyModel(this);
    treeProxy_->setSourceModel(model_);
    treeProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    treeProxy_->setFilterKeyColumn(0);
    treeProxy_->setSortRole(FileSystemModel::Roles::SortRole);
    tree_->setModel(treeProxy_);

    rightSplitter_ = new QSplitter(Qt::Vertical, splitter_);
    tabs_ = new QTabWidget(rightSplitter_);
    detailsView_ = new QTableView(tabs_);
    detailsView_->setAccessibleName(QStringLiteral("Details table"));
    detailsView_->setSortingEnabled(true);
    detailsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    detailsModel_ = new DetailsTableModel(model_, this);
    detailsProxy_ = new QSortFilterProxyModel(this);
    detailsProxy_->setSourceModel(detailsModel_);
    detailsProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    detailsProxy_->setFilterKeyColumn(0);
    detailsProxy_->setSortRole(FileSystemModel::Roles::SortRole);
    detailsView_->setModel(detailsProxy_);
    detailsView_->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(detailsView_, QStringLiteral("Details"));
    bar_ = new BarWidget(model_);
    auto* barScroll = new QScrollArea(tabs_);
    barScroll->setWidget(bar_);
    barScroll->setWidgetResizable(true);
    tabs_->addTab(barScroll, QStringLiteral("Bars"));
    // Placeholders for Milestone 2 analytics (kept disabled, not removed,
    // so the tab contract is visible without fake data).
    for (const QString& name :
         {QStringLiteral("Extensions"), QStringLiteral("Users"),
          QStringLiteral("Age of Files"), QStringLiteral("Top Files")}) {
        auto* ph = new QLabel(QStringLiteral("%1 view arrives in Milestone 2.").arg(name),
                              tabs_);
        ph->setAlignment(Qt::AlignCenter);
        ph->setEnabled(false);
        tabs_->addTab(ph, name);
    }

    splitter_->addWidget(tree_);
    splitter_->addWidget(rightSplitter_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 2);
    treemap_ = new TreemapWidget(model_, rightSplitter_);
    rightSplitter_->addWidget(tabs_);
    rightSplitter_->addWidget(treemap_);
    rightSplitter_->setStretchFactor(0, 3);
    rightSplitter_->setStretchFactor(1, 2);
    layout->addWidget(splitter_, 1);
    setCentralWidget(central);

    // Status bar: state visibility per AGENTS.md §3 (never imply success early)
    statusLabel_ = new QLabel(QStringLiteral("Ready"), this);
    drivesLabel_ = new QLabel(this);
    countsLabel_ = new QLabel(this);
    freeSpaceLabel_ = new QLabel(this);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setVisible(false);
    progress_->setTextVisible(false);
    progress_->setFixedWidth(120);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addWidget(drivesLabel_);
    statusBar()->addWidget(countsLabel_);
    statusBar()->addWidget(freeSpaceLabel_);
    statusBar()->addPermanentWidget(progress_);

    buildMenus();
    buildToolbar();

    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowseTarget);
    connect(startStopBtn_, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(quickSearch_, &QLineEdit::textChanged, this, &MainWindow::onQuickSearchChanged);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { onTreeSelectionChanged(); });
    // Treemap follows the model through a debounce (live scans must not
    // repaint-storm); selection changes re-root it immediately.
    connect(model_, &FileSystemModel::rowsInserted, treemap_, &TreemapWidget::refreshSoon);
    connect(model_, &FileSystemModel::dataChanged, treemap_, &TreemapWidget::refreshSoon);
    connect(model_, &FileSystemModel::modelResetForNewScan, this,
            [this](const QString&) { treemap_->setRootId(0); });
    connect(model_, &FileSystemModel::rowsInserted, bar_, &BarWidget::refreshSoon);
    connect(model_, &FileSystemModel::dataChanged, bar_, &BarWidget::refreshSoon);
    connect(model_, &FileSystemModel::modelResetForNewScan, this,
            [this](const QString&) { bar_->setRootId(0); });
    connect(bar_, &BarWidget::barClicked, this, [this](quint32 id) { selectTreeNode(id); });
    connect(bar_, &BarWidget::barDoubleClicked, this, [this](quint32 id) {
        selectTreeNode(id);
        if (model_->isDirectoryNode(id)) {
            const QModelIndex proxy = treeProxy_->mapFromSource(model_->indexForNodeId(id));
            if (proxy.isValid()) {
                tree_->expand(proxy);
            }
        }
    });
    connect(treemap_, &TreemapWidget::tileClicked, this,
            [this](quint32 id) { selectTreeNode(id); });
    connect(treemap_, &TreemapWidget::tileDoubleClicked, this, [this](quint32 id) {
        selectTreeNode(id);
        if (model_->isDirectoryNode(id)) {
            const QModelIndex proxy = treeProxy_->mapFromSource(model_->indexForNodeId(id));
            if (proxy.isValid()) {
                tree_->expand(proxy);
            }
        }
    });
    connect(scanner_, &ScannerService::scanFinished, this, &MainWindow::onScanFinished);
    connect(scanner_, &ScannerService::scanProgress, this,
            [this](const QString&, const QString& p) { statusLabel_->setText(p); });
    connect(scanner_, &ScannerService::errorOccurred, this,
            [this](const QString&, const QString& err, bool fatal) {
                if (fatal) {
                    QMessageBox::critical(this, QStringLiteral("Scan Error"), err);
                } else if (skippedPaths_.size() < kMaxSkippedPaths) {
                    skippedPaths_.push_back(err);
                } else {
                    ++skippedOverflow_;
                }
            });

    // Expert keyboard paths: Ctrl+L focuses the target box (browser-style
    // location shortcut). Ctrl+O/Ctrl+F live on their menu actions above.
    auto* focusTargetAct = new QAction(this);
    focusTargetAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(focusTargetAct, &QAction::triggered, this, [this] {
        targetEdit_->setFocus();
        targetEdit_->selectAll();
    });
    addAction(focusTargetAct);

    // Drive overview
    QStringList drives{};
    for (const QStorageInfo& vol : QStorageInfo::mountedVolumes()) {
        if (vol.isValid() && !vol.isReadOnly()) {
            drives << vol.rootPath();
        }
        if (drives.size() >= 4) {
            break;
        }
    }
    drivesLabel_->setText(QStringLiteral("Drives: %1").arg(drives.join(QStringLiteral(", "))));
    const QStorageInfo home{QDir::homePath()};
    if (home.isValid()) {
        freeSpaceLabel_->setText(
            QStringLiteral("Free: %1").arg(formatBytes(static_cast<quint64>(home.bytesAvailable()),
                                                       QStringLiteral("Auto"))));
    }

    restoreWindowState();
    onTreeSelectionChanged();
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    auto* targetAct =
        file->addAction(QStringLiteral("Select Target…"), this, &MainWindow::onBrowseTarget);
    targetAct->setShortcut(QKeySequence::Open); // Ctrl+O
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit);

    auto* scan = menuBar()->addMenu(QStringLiteral("&Scan"));
    auto* toggleAct = scan->addAction(QStringLiteral("Start/Stop"), this, &MainWindow::onStartStop);
    toggleAct->setShortcut(QKeySequence(QStringLiteral("F5")));

    auto* view = menuBar()->addMenu(QStringLiteral("&View"));
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    const QList<QPair<QString, ThemeManager::Mode>> modes{
        {QStringLiteral("System Theme"), ThemeManager::Mode::System},
        {QStringLiteral("Light"), ThemeManager::Mode::Light},
        {QStringLiteral("Dark"), ThemeManager::Mode::Dark},
    };
    for (const auto& [label, mode] : modes) {
        auto* act = view->addAction(label);
        act->setCheckable(true);
        act->setChecked(themes_ != nullptr && themes_->mode() == mode);
        act->setData(static_cast<int>(mode));
        group->addAction(act);
        connect(act, &QAction::triggered, this, &MainWindow::onThemeAction);
    }
    auto* findAct = view->addAction(QStringLiteral("&Find…"));
    findAct->setShortcut(QKeySequence::Find); // Ctrl+F
    connect(findAct, &QAction::triggered, this, [this] { quickSearch_->setFocus(); });
}

void MainWindow::buildToolbar()
{
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setMovable(false);
    auto* exportAct = tb->addAction(QStringLiteral("Export"));
    exportAct->setEnabled(false); // Milestone 4
    exportAct->setToolTip(QStringLiteral("Multi-format export arrives in Milestone 4"));
    auto* optionsAct = tb->addAction(QStringLiteral("Options"));
    optionsAct->setEnabled(false);
    optionsAct->setToolTip(QStringLiteral("Age intervals & export options arrive in Milestone 2/4"));
}

void MainWindow::onBrowseTarget()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select folder to scan"), targetEdit_->text());
    if (!dir.isEmpty()) {
        targetEdit_->setText(dir);
    }
}

void MainWindow::onStartStop()
{
    if (!scanning_) {
        const QString target = targetEdit_->text().trimmed();
        if (target.isEmpty()) {
            statusLabel_->setText(QStringLiteral("Select a target folder first."));
            return;
        }
        model_->onScanStarted(QString{}, target);
        activeTaskId_ = scanner_->startScan(target);
        scanning_ = true;
        skippedPaths_.clear();
        skippedOverflow_ = 0;
        // Freeze live resorting/filtering while rows stream in: per-insert
        // re-sorts over a million-row model would stall the event loop.
        // One full re-sort happens on finish instead.
        treeProxy_->setDynamicSortFilter(false);
        detailsProxy_->setDynamicSortFilter(false);
        startStopBtn_->setText(QStringLiteral("Stop"));
        statusLabel_->setText(QStringLiteral("Scanning…"));
        countsLabel_->setText(QString{});
        progress_->setVisible(true);
    } else {
        scanner_->cancelScan(activeTaskId_);
        statusLabel_->setText(QStringLiteral("Stopping…"));
        startStopBtn_->setEnabled(false);
    }
}

void MainWindow::onScanFinished(const QString& taskId, quint64 files, quint64 dirs, quint64 bytes)
{
    if (taskId != activeTaskId_) {
        return;
    }
    scanning_ = false;
    startStopBtn_->setText(QStringLiteral("Start"));
    startStopBtn_->setEnabled(true);
    progress_->setVisible(false);
    treeProxy_->setDynamicSortFilter(true);
    detailsProxy_->setDynamicSortFilter(true);
    const qsizetype skippedTotal = skippedPaths_.size() + skippedOverflow_;
    if (skippedTotal > 0) {
        statusLabel_->setText(QStringLiteral("Ready (%1 skipped)").arg(skippedTotal));
    } else {
        statusLabel_->setText(QStringLiteral("Ready"));
    }
    countsLabel_->setText(QStringLiteral("Files: %1  Folders: %2  Total: %3")
                              .arg(files)
                              .arg(dirs)
                              .arg(formatBytes(bytes, unitsCombo_->currentText())));
    onTreeSelectionChanged();
    if (skippedTotal > 0) {
        QString details = skippedPaths_.join(QStringLiteral("\n"));
        if (skippedOverflow_ > 0) {
            details += QStringLiteral("\n… and %1 more.").arg(skippedOverflow_);
        }
        QMessageBox summary(QMessageBox::Warning, QStringLiteral("Scan completed with skipped items"),
                            QStringLiteral("%1 director%2 could not be scanned and %3 excluded "
                                           "from the results. Re-run as administrator or exclude "
                                           "system folders to reduce skips.")
                                .arg(skippedTotal)
                                .arg(skippedTotal == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                                .arg(skippedTotal == 1 ? QStringLiteral("is") : QStringLiteral("are")),
                            QMessageBox::Ok, this);
        summary.setDetailedText(details);
        summary.exec();
    }
}

void MainWindow::onTreeSelectionChanged()
{
    const QModelIndex cur = tree_->currentIndex();
    const QModelIndex src = treeProxy_->mapToSource(cur);
    detailsModel_->setSelectedNode(src.isValid() ? src : QModelIndex{});
    // The treemap shows a folder's children: re-root on folder selection,
    // keep the parent context when a file is picked.
    const quint32 id = model_->nodeIdForIndex(src);
    if (model_->isDirectoryNode(id)) {
        treemap_->setRootId(id);
        bar_->setRootId(id);
    }
}

void MainWindow::selectTreeNode(quint32 nodeId)
{
    const QModelIndex src = model_->indexForNodeId(nodeId);
    if (!src.isValid()) {
        return;
    }
    const QModelIndex proxy = treeProxy_->mapFromSource(src);
    if (proxy.isValid()) {
        tree_->scrollTo(proxy);
        tree_->setCurrentIndex(proxy);
    }
}

void MainWindow::onQuickSearchChanged(const QString& text)
{
    treeProxy_->setFilterFixedString(text);
    detailsProxy_->setFilterFixedString(text);
}

void MainWindow::onThemeAction()
{
    if (auto* act = qobject_cast<QAction*>(sender()); act != nullptr && themes_ != nullptr) {
        themes_->setMode(static_cast<ThemeManager::Mode>(act->data().toInt()));
    }
}

void MainWindow::restoreWindowState()
{
    QSettings s(QStringLiteral("UniversalStorageAnalyzer"),
                QStringLiteral("UniversalStorageAnalyzer"));
    restoreGeometry(s.value(QStringLiteral("main/geometry")).toByteArray());
    QMainWindow::restoreState(s.value(QStringLiteral("main/windowState")).toByteArray());
    splitter_->restoreState(s.value(QStringLiteral("main/splitter")).toByteArray());
    rightSplitter_->restoreState(s.value(QStringLiteral("main/rightSplitter")).toByteArray());
    const QString unit = s.value(QStringLiteral("main/units"), QStringLiteral("Auto")).toString();
    unitsCombo_->setCurrentText(unit);
    targetEdit_->setText(
        s.value(QStringLiteral("main/target"), QDir::homePath()).toString());
}

void MainWindow::saveWindowState()
{
    QSettings s(QStringLiteral("UniversalStorageAnalyzer"),
                QStringLiteral("UniversalStorageAnalyzer"));
    s.setValue(QStringLiteral("main/geometry"), saveGeometry());
    s.setValue(QStringLiteral("main/windowState"), QMainWindow::saveState());
    s.setValue(QStringLiteral("main/splitter"), splitter_->saveState());
    s.setValue(QStringLiteral("main/rightSplitter"), rightSplitter_->saveState());
    s.setValue(QStringLiteral("main/units"), unitsCombo_->currentText());
    s.setValue(QStringLiteral("main/target"), targetEdit_->text());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveWindowState();
    QMainWindow::closeEvent(event);
}

QString MainWindow::formatBytes(quint64 bytes, const QString& unit)
{
    double v{static_cast<double>(bytes)};
    QString suffix{unit};
    if (unit == QStringLiteral("Auto")) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int idx{0};
        while (v >= 1024.0 && idx < 4) {
            v /= 1024.0;
            ++idx;
        }
        suffix = QString::fromUtf8(units[idx]);
    } else {
        if (unit == QStringLiteral("KB")) {
            v /= 1024.0;
        } else if (unit == QStringLiteral("MB")) {
            v /= 1024.0 * 1024.0;
        } else if (unit == QStringLiteral("GB")) {
            v /= 1024.0 * 1024.0 * 1024.0;
        }
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 2).arg(suffix);
}
