#pragma once

#include <QAbstractTableModel>
#include <QPersistentModelIndex>

#include "../models/FileSystemModel.h"

// Read-only projection of the selected tree node's children for the
// right-pane Details tab. Fed by selection changes; sortable via
// QSortFilterProxyModel in MainWindow.
//
// The selection is a QPersistentModelIndex: a scan reset invalidates it
// instead of leaving a dangling pointer into the old tree, and every
// accessor guards validity first.
class DetailsTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit DetailsTableModel(FileSystemModel* source, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

public slots:
    void setSelectedNode(const QModelIndex& sourceIndex);

private:
    FileSystemModel* source_{nullptr};
    QPersistentModelIndex selected_{};
};
