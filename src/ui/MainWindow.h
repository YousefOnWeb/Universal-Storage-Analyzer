#pragma once

#include <QMainWindow>

#include "../models/DetailsTableModel.h"
#include "../models/FileSystemModel.h"
#include "../services/ScannerService.h"

#include <QComboBox>
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

private:
    void buildMenus();
    void buildToolbar();
    void restoreWindowState();
    void saveWindowState();
    static QString formatBytes(quint64 bytes, const QString& unit);

    FileSystemModel* model_{nullptr};
    ScannerService* scanner_{nullptr};
    ThemeManager* themes_{nullptr};

    QTreeView* tree_{nullptr};
    QSortFilterProxyModel* treeProxy_{nullptr};
    QTableView* detailsView_{nullptr};
    QSortFilterProxyModel* detailsProxy_{nullptr};
    DetailsTableModel* detailsModel_{nullptr};
    QTabWidget* tabs_{nullptr};
    QSplitter* splitter_{nullptr};
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

protected:
    void closeEvent(QCloseEvent* event) override;
};
