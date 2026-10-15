#include "MainWindow.h"
#include "BarWidget.h"
#include "OptionsDialog.h"
#include "PieWidget.h"
#include "ThemeManager.h"
#include "TreemapWidget.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStorageInfo>
#include <QToolBar>
#include <QVBoxLayout>

#include <functional>

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

    treeProxy_ = new TreeFilterProxyModel(this);
    treeProxy_->setSourceModel(model_);
    treeProxy_->setSortRole(FileSystemModel::Roles::SortRole);
    tree_->setModel(treeProxy_);
    // Corrected requirement: the left pane navigates (names + hierarchy
    // only — sizes, counts, and timestamps live on the right) and is
    // ALWAYS Size-descending — a fixed descriptor, not a user setting.
    // Comparators read the numeric sort role; ties keep insertion order
    // (stable sort), so a given input always orders identically. The
    // header is not clickable and any programmatic deviation is forced
    // back (see the enforcer below); the details pane sorts
    // independently with the same default.
    for (int c = FileSystemModel::ColSize; c < FileSystemModel::ColCount; ++c) {
        tree_->hideColumn(c);
    }
    tree_->header()->setSectionsClickable(false);
    // View-level sorts so the header indicators reflect the descriptor
    // from the start (proxy-level sorts would leave them blank).
    tree_->sortByColumn(FileSystemModel::ColSize, Qt::DescendingOrder);
    connect(tree_->header(), &QHeaderView::sortIndicatorChanged, this,
            [this](int column, Qt::SortOrder order) {
                if (column != FileSystemModel::ColSize || order != Qt::DescendingOrder) {
                    const QSignalBlocker block(tree_->header());
                    tree_->sortByColumn(FileSystemModel::ColSize, Qt::DescendingOrder);
                }
            });

    rightSplitter_ = new QSplitter(Qt::Vertical, splitter_);
    tabs_ = new QTabWidget(rightSplitter_);
    detailsView_ = new QTableView(tabs_);
    detailsView_->setAccessibleName(QStringLiteral("Details table"));
    detailsView_->setObjectName(QStringLiteral("Details"));
    detailsView_->setSortingEnabled(true);
    detailsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    detailsModel_ = new DetailsTableModel(model_, this);
    detailsProxy_ = new QSortFilterProxyModel(this);
    detailsProxy_->setSourceModel(detailsModel_);
    detailsProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    detailsProxy_->setFilterKeyColumn(0);
    detailsProxy_->setSortRole(FileSystemModel::Roles::SortRole);
    detailsView_->setModel(detailsProxy_);
    detailsView_->sortByColumn(FileSystemModel::ColSize, Qt::DescendingOrder);
    detailsView_->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(detailsView_, QStringLiteral("Details"));
    bar_ = new BarWidget(model_);
    auto* barScroll = new QScrollArea(tabs_);
    barScroll->setWidget(bar_);
    barScroll->setWidgetResizable(true);
    tabs_->addTab(barScroll, QStringLiteral("Bars"));
    pie_ = new PieWidget(model_, tabs_);
    tabs_->addTab(pie_, QStringLiteral("Pie"));
    extView_ = makeStatsTab(QStringLiteral("Extensions"), extModel_, extProxy_);
    topView_ = makeStatsTab(QStringLiteral("Top Files"), topModel_, topProxy_);
    usersView_ = makeStatsTab(QStringLiteral("Users"), usersModel_, usersProxy_);
    ageView_ = makeStatsTab(QStringLiteral("Age of Files"), ageModel_, ageProxy_);
    extModel_->setHeaders({QStringLiteral("Extension"), QStringLiteral("Bytes"),
                           QStringLiteral("Share"), QStringLiteral("Files")});
    topModel_->setHeaders({QStringLiteral("Name"), QStringLiteral("Size"),
                           QStringLiteral("Path")});
    usersModel_->setHeaders({QStringLiteral("Owner"), QStringLiteral("Bytes"),
                             QStringLiteral("Share"), QStringLiteral("Files")});
    ageModel_->setHeaders({QStringLiteral("Age"), QStringLiteral("Files"),
                           QStringLiteral("Bytes")});
    tabs_->setTabToolTip(
        tabs_->indexOf(usersView_),
        QStringLiteral("File owners where the backend captures them; "
                       "(unknown) where it cannot (e.g. Windows native enumeration)"));

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
    connect(model_, &FileSystemModel::rowsInserted, pie_, &PieWidget::refreshSoon);
    connect(model_, &FileSystemModel::dataChanged, pie_, &PieWidget::refreshSoon);
    connect(model_, &FileSystemModel::modelResetForNewScan, this,
            [this](const QString&) { pie_->setRootId(0); });
    connect(pie_, &PieWidget::pieClicked, this, [this](quint32 id) { selectTreeNode(id); });
    connect(pie_, &PieWidget::pieDoubleClicked, this, [this](quint32 id) {
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
    connect(scanner_, &ScannerService::analyticsUpdated, this,
            &MainWindow::onAnalyticsUpdated);
    connect(scanner_, &ScannerService::throttleChanged, this,
            [this](const QString&, bool throttled) {
                statusLabel_->setText(throttled ? QStringLiteral("Throttled (disk busy)")
                                                : QStringLiteral("Scanning…"));
            });
    connect(topView_, &QTableView::clicked, this, [this](const QModelIndex& proxyIdx) {
        const QModelIndex src = topProxy_->mapToSource(proxyIdx);
        if (src.isValid()) {
            selectTreeNode(topModel_->rowNodeId(src.row()));
        }
    });
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
    // Notices (backend downgrades) are transient status text: they are
    // neither fatal nor skipped directories.
    connect(scanner_, &ScannerService::noticeOccurred, this,
            [this](const QString&, const QString& msg) { statusLabel_->setText(msg); });

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
            QStringLiteral("Free: %1").arg(usa::models::formatSizeBytes(static_cast<quint64>(home.bytesAvailable()),
                                                       QStringLiteral("Auto"))));
    }

    restoreWindowState();
    applyStartupTarget();
    pie_->setTargetPath(targetEdit_->text());
    applyOptionsToEngine(); // push saved options before the first scan
    // Spec 006b: one selector drives every rendered size (model
    // strings, charts, panes, footer). Sync renderers to the restored
    // unit explicitly — setCurrentText only signals on real changes —
    // then follow user changes the same way.
    connect(unitsCombo_, &QComboBox::currentTextChanged, this,
            [this](const QString& unit) { applyUnits(unit); });
    applyUnits(unitsCombo_->currentText());
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

    auto* tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    auto* optionsAct = tools->addAction(QStringLiteral("&Options…"), this, &MainWindow::onOptions);
    optionsAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
}

void MainWindow::buildToolbar()
{
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setMovable(false);
    auto* exportAct = tb->addAction(QStringLiteral("Export"));
    exportAct->setEnabled(false); // Milestone 4
    exportAct->setToolTip(QStringLiteral("Multi-format export arrives in Milestone 4"));
    auto* optionsAct = tb->addAction(QStringLiteral("Options"));
    optionsAct->setToolTip(QStringLiteral("Scan and display options"));
    connect(optionsAct, &QAction::triggered, this, &MainWindow::onOptions);
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
        pie_->setTargetPath(target);
        scanning_ = true;
        scanWallT_.start();
        skippedPaths_.clear();
        skippedOverflow_ = 0;
        // Freeze live resorting/filtering while rows stream in: per-insert
        // re-sorts over a million-row model would stall the event loop.
        // Proxies stay dynamically unsorted (no finish-time re-sort either).
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
    // No finish-time re-sort: with no further model changes pending, the
    // proxies have nothing to re-sort against, and sorting on demand
    // (header clicks) works regardless of dynamicSortFilter. Re-enabling
    // here only buys a multi-second full sort before "Ready" paints
    // (measured 2.8 s on a 2M-row model). Still disabled from scan start;
    // the next scan re-applies it.
    qInfo() << "Scan UI finalize: model insert" << (model_->insertUsec() / 1000ULL)
            << "ms, click-to-Ready" << scanWallT_.elapsed() << "ms";
    const qsizetype skippedTotal = skippedPaths_.size() + skippedOverflow_;
    if (skippedTotal > 0) {
        statusLabel_->setText(QStringLiteral("Ready (%1 skipped)").arg(skippedTotal));
    } else {
        statusLabel_->setText(QStringLiteral("Ready"));
    }
    countsLabel_->setText(QStringLiteral("Files: %1  Folders: %2  Total: %3")
                              .arg(files)
                              .arg(dirs)
                              .arg(usa::models::formatSizeBytes(bytes, unitsCombo_->currentText())));
    lastFiles_ = files;
    lastDirs_ = dirs;
    lastBytes_ = bytes;
    hasResult_ = true;
    // Corrected requirement: the tree is ALWAYS Size-descending. Live
    // inserts stream frozen (per-insert re-sorts would stall the loop),
    // so the finish applies one full sort plus dynamic ordering for the
    // sweep drain still in flight — synchronous, measured in seconds on
    // million-row models, and the honest price of the invariant. The
    // enforcer (ctor) keeps every later change pinned to it. Details
    // re-enables dynamic ordering with the same default but stays
    // freely user-sortable.
    treeProxy_->setDynamicSortFilter(true);
    tree_->sortByColumn(FileSystemModel::ColSize, Qt::DescendingOrder);
    detailsProxy_->setDynamicSortFilter(true);
    detailsView_->sortByColumn(FileSystemModel::ColSize, Qt::DescendingOrder);
    // Spec 005a: the finished scan selects its volume root, so the
    // right pane lists the volume's direct contents. Guarded by the
    // task check above; a filtered-out root simply selects nothing.
    selectTreeNode(0);
    const QModelIndex rootProxy = treeProxy_->mapFromSource(model_->indexForNodeId(0));
    if (rootProxy.isValid()) {
        tree_->expand(rootProxy);
    }
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
        pie_->setRootId(id);
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
    treeProxy_->setSearchText(text);
    detailsProxy_->setFilterFixedString(text);
}

void MainWindow::onThemeAction()
{
    if (auto* act = qobject_cast<QAction*>(sender()); act != nullptr && themes_ != nullptr) {
        themes_->setMode(static_cast<ThemeManager::Mode>(act->data().toInt()));
    }
}

void MainWindow::onOptions()
{
    OptionsDialog dlg{this};
    if (dlg.exec() == QDialog::Accepted) {
        applyOptionsToEngine();
        refreshAgePane();
    }
}

void MainWindow::applyStartupTarget()
{
    const QString mode = OptionsDialog::loadStartup();
    if (mode == QStringLiteral("home")) {
        targetEdit_->setText(QDir::homePath());
    } else if (mode == QStringLiteral("ask")) {
        targetEdit_->clear();
    }
    // "last" keeps the restored target untouched.
}

void MainWindow::applyOptionsToEngine()
{
    scanner_->applyOptions(OptionsDialog::loadBucketsCsv(), OptionsDialog::loadTopN(),
                           OptionsDialog::loadThrottle());
}

void MainWindow::refreshAgePane()
{
    // Re-bucket from in-memory mtimes: no I/O, instant on any scan.
    const std::vector<std::uint64_t> edges = OptionsDialog::loadEdges();
    std::vector<std::uint64_t> files(edges.size() + 1, 0);
    std::vector<std::uint64_t> bytes(edges.size() + 1, 0);
    const std::uint64_t now =
        static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch());
    std::function<void(quint32)> walk = [&](quint32 id) {
        for (const auto& child : model_->childEntries(id)) {
            if (child.isDirectory) {
                walk(child.id);
                continue;
            }
            const std::uint64_t mtime = model_->lastModifiedTs(child.id);
            if (mtime == 0 || mtime > now) {
                continue; // unknown or clock-skewed: never bucket by guess
            }
            const std::uint64_t ageDays = (now - mtime) / 86400ULL;
            std::size_t bucket{edges.size()};
            for (std::size_t i{0}; i < edges.size(); ++i) {
                if (ageDays < edges[i]) {
                    bucket = i;
                    break;
                }
            }
            ++files[bucket];
            bytes[bucket] += child.logicalSize;
        }
    };
    walk(0);
    QVector<StatsRow> rows{};
    for (std::size_t i{0}; i < files.size(); ++i) {
        StatsRow r{};
        r.text = {ageBucketLabel(i, edges), QLocale{}.toString(files[i]),
                  usa::models::formatSizeBytes(bytes[i], unitsCombo_->currentText())};
        r.keys = {ageBucketLabel(i, edges), files[i], bytes[i]};
        rows.push_back(std::move(r));
    }
    ageModel_->setRows(std::move(rows));
}

void MainWindow::refreshFooter()
{
    // Spec 006b: footer honors the unit selector like every other
    // rendered size. Totals need a completed scan; free space is a
    // live query and refreshes unconditionally.
    if (hasResult_) {
        countsLabel_->setText(QStringLiteral("Files: %1  Folders: %2  Total: %3")
                                  .arg(lastFiles_)
                                  .arg(lastDirs_)
                                  .arg(usa::models::formatSizeBytes(lastBytes_,
                                                                    unitsCombo_->currentText())));
    }
    const QStorageInfo home{QDir::homePath()};
    if (home.isValid()) {
        freeSpaceLabel_->setText(QStringLiteral("Free: %1").arg(
            usa::models::formatSizeBytes(static_cast<quint64>(home.bytesAvailable()),
                                         unitsCombo_->currentText())));
    }
}

void MainWindow::applyUnits(const QString& unit)
{
    // Spec 006b: one selector drives model strings, charts, panes,
    // age buckets, and the footer. Model + charts re-render from
    // stored raw values; panes rebuild from cached snapshots.
    model_->setSizeUnit(unit);
    bar_->setSizeUnit(unit);
    pie_->setSizeUnit(unit);
    treemap_->setSizeUnit(unit);
    if (hasAnalytics_) {
        projectAnalytics(lastAnalytics_);
    }
    refreshAgePane();
    refreshFooter();
}

QTableView* MainWindow::makeStatsTab(const QString& title, StatsTableModel*& model,
                                     QSortFilterProxyModel*& proxy)
{
    model = new StatsTableModel(this);
    proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(model);
    proxy->setSortRole(FileSystemModel::Roles::SortRole);
    auto* view = new QTableView(tabs_);
    view->setAccessibleName(title);
    view->setObjectName(title);
    view->setSortingEnabled(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setModel(proxy);
    view->horizontalHeader()->setStretchLastSection(true);
    tabs_->addTab(view, title);
    return view;
}

QString MainWindow::shareText(double pct) const
{
    return QString::number(pct, 'f', 1) + QStringLiteral("%");
}

void MainWindow::onAnalyticsUpdated(const QString&, const AnalyticsData& data)
{
    lastAnalytics_ = data;
    hasAnalytics_ = true;
    projectAnalytics(lastAnalytics_);
}

void MainWindow::projectAnalytics(const AnalyticsData& data)
{
    const QString units = unitsCombo_->currentText();
    {
        QVector<StatsRow> rows{};
        rows.reserve(data.extensions.size());
        for (const auto& row : data.extensions) {
            StatsRow r{};
            r.text = {row.extension, usa::models::formatSizeBytes(row.bytes, units), shareText(row.sharePct),
                      QLocale{}.toString(row.files)};
            r.keys = {row.extension, row.bytes,
                      static_cast<quint64>(row.sharePct * 10.0),
                      row.files};
            rows.push_back(std::move(r));
        }
        extModel_->setRows(std::move(rows));
    }
    {
        QVector<StatsRow> rows{};
        rows.reserve(data.top.size());
        for (const auto& row : data.top) {
            StatsRow r{};
            r.text = {row.name, usa::models::formatSizeBytes(row.bytes, units), row.path};
            r.keys = {row.name, row.bytes, row.path};
            r.nodeId = row.nodeId;
            rows.push_back(std::move(r));
        }
        topModel_->setRows(std::move(rows));
    }
    {
        QVector<StatsRow> rows{};
        rows.reserve(data.owners.size());
        for (const auto& row : data.owners) {
            StatsRow r{};
            r.text = {row.owner, usa::models::formatSizeBytes(row.bytes, units), shareText(row.sharePct),
                      QLocale{}.toString(row.files)};
            r.keys = {row.owner, row.bytes,
                      static_cast<quint64>(row.sharePct * 10.0),
                      row.files};
            rows.push_back(std::move(r));
        }
        usersModel_->setRows(std::move(rows));
    }
    {
        QVector<StatsRow> rows{};
        rows.reserve(data.age.size());
        for (const auto& row : data.age) {
            StatsRow r{};
            r.text = {row.label, QLocale{}.toString(row.files),
                      usa::models::formatSizeBytes(row.bytes, units)};
            r.keys = {row.label, row.files, row.bytes};
            rows.push_back(std::move(r));
        }
        ageModel_->setRows(std::move(rows));
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
    // Spec 005d (+006a column insertion): the descriptor persists with
    // a version tag. Unversioned values predate the share column, when
    // section 5 meant Last Modified and only 0-5 existed: migrate those
    // (5 -> Modified, 0-4 identical), drop anything else. Without this,
    // a stale saved 5 would silently rebind to "% of Parent".
    static constexpr int kDetailsSortVersion = 1;
    const int savedCol = s.value(QStringLiteral("main/detailsSortColumn"), -1).toInt();
    const int savedOrder = s.value(QStringLiteral("main/detailsSortOrder"), -1).toInt();
    const int savedVer = s.value(QStringLiteral("main/detailsSortVersion"), 0).toInt();
    const bool orderOk =
        savedOrder == static_cast<int>(Qt::AscendingOrder)
        || savedOrder == static_cast<int>(Qt::DescendingOrder);
    int sortCol = FileSystemModel::ColSize;
    Qt::SortOrder sortOrder = Qt::DescendingOrder;
    if (savedVer == kDetailsSortVersion && savedCol >= 0
        && savedCol < FileSystemModel::ColCount && orderOk) {
        sortCol = savedCol;
        sortOrder = static_cast<Qt::SortOrder>(savedOrder);
    } else if (savedVer == 0 && savedCol >= 0 && savedCol < 6 && orderOk) {
        // Pre-share layout had six columns (0-5); only section 5 moved.
        sortCol = (savedCol == 5) ? FileSystemModel::ColModified : savedCol;
        sortOrder = static_cast<Qt::SortOrder>(savedOrder);
    }
    detailsView_->sortByColumn(sortCol, sortOrder);
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
    s.setValue(QStringLiteral("main/detailsSortColumn"),
               detailsView_->horizontalHeader()->sortIndicatorSection());
    s.setValue(QStringLiteral("main/detailsSortOrder"),
               static_cast<int>(detailsView_->horizontalHeader()->sortIndicatorOrder()));
    s.setValue(QStringLiteral("main/detailsSortVersion"), 1);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveWindowState();
    QMainWindow::closeEvent(event);
}
