#pragma once

#include <QMainWindow>

#include "../models/DetailsTableModel.h"
#include "../models/FileSystemModel.h"
#include "../services/ScannerService.h"
#include "BarWidget.h"
#include "PieWidget.h"
#include "../models/StatsTableModel.h"
#include "../models/TreeFilterProxy.h"
#include "TreemapWidget.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>

class ThemeManager;

// Sprint 1 main window: dual-pane layout (tree left, details tabs right),
// toolbar (target/start-stop/units/export/options), quick search, status bar.
// All scan I/O is async via ScannerService signals; widgets are only
// touched on the GUI thread.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(FileSystemModel* model, ScannerService* scanner, ThemeManager* themes,
               QWidget* parent = nullptr);

private slots:
    void onBrowseTarget();
    void onStartStop();
    void onScanFinished(const QString& taskId, quint64 files, quint64 dirs, quint64 bytes);
    void onTreeSelectionChanged();
    void onQuickSearchChanged(const QString& text);
    void onThemeAction();
    void onOptions();
    void onAnalyticsUpdated(const QString& taskId, const AnalyticsData& data);
    void onExtensionPicked();

public slots:
    // Re-buckets the Age pane from in-memory mtimes (no I/O). Runs after
    // the options dialog changes buckets and on demand otherwise.
    void refreshAgePane();

private:
    void buildMenus();
    void buildToolbar();
    void restoreWindowState();
    void saveWindowState();
    void selectTreeNode(quint32 nodeId);
    void applyOptionsToEngine();
    void applyStartupTarget();
    // Mirrors a header sort change onto the other view (spec 005b: one
    // shared descriptor for tree + details). Callers pass the header
    // that changed; signals are blocked while the mirror applies.
    void applySharedSort(QHeaderView* changed, int column, Qt::SortOrder order);
    static QString formatBytes(quint64 bytes, const QString& unit);
    QTableView* makeStatsTab(const QString& title, StatsTableModel*& model,
                             QSortFilterProxyModel*& proxy);
    QString shareText(double pct) const;

    FileSystemModel* model_{nullptr};
    ScannerService* scanner_{nullptr};
    ThemeManager* themes_{nullptr};

    QTreeView* tree_{nullptr};
    TreeFilterProxyModel* treeProxy_{nullptr};
    QTableView* detailsView_{nullptr};
    QSortFilterProxyModel* detailsProxy_{nullptr};
    DetailsTableModel* detailsModel_{nullptr};
    QTabWidget* tabs_{nullptr};
    QSplitter* splitter_{nullptr};
    QSplitter* rightSplitter_{nullptr};
    TreemapWidget* treemap_{nullptr};
    BarWidget* bar_{nullptr};
    PieWidget* pie_{nullptr};
    QTableView* extView_{nullptr};
    QTableView* topView_{nullptr};
    QTableView* usersView_{nullptr};
    QTableView* ageView_{nullptr};
    StatsTableModel* extModel_{nullptr};
    StatsTableModel* topModel_{nullptr};
    StatsTableModel* usersModel_{nullptr};
    StatsTableModel* ageModel_{nullptr};
    QSortFilterProxyModel* extProxy_{nullptr};
    QSortFilterProxyModel* topProxy_{nullptr};
    QSortFilterProxyModel* usersProxy_{nullptr};
    QSortFilterProxyModel* ageProxy_{nullptr};
    QLineEdit* quickSearch_{nullptr};
    QLineEdit* targetEdit_{nullptr};
    QPushButton* startStopBtn_{nullptr};
    QComboBox* unitsCombo_{nullptr};
    QLabel* statusLabel_{nullptr};
    QLabel* drivesLabel_{nullptr};
    QLabel* countsLabel_{nullptr};
    QLabel* freeSpaceLabel_{nullptr};
    QProgressBar* progress_{nullptr};

    QString activeTaskId_{};
    bool scanning_{false};
    QElapsedTimer scanWallT_{}; // click-to-Ready wall time for speed diagnosis
    // Non-fatal scan errors collected during a scan (fatal ones pop up
    // immediately). Shown together in one summary dialog on finish; capped
    // so pathological volumes cannot grow the list without bound.
    static constexpr int kMaxSkippedPaths{1000};
    QStringList skippedPaths_{};
    int skippedOverflow_{0};

protected:
    void closeEvent(QCloseEvent* event) override;
};
