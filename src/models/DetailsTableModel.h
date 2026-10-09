#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>

#include "../models/FileSystemModel.h"

// Read-only projection of the selected tree node's children for the
// right-pane Details tab. Fed by selection changes; sortable via
// QSortFilterProxyModel in MainWindow.
//
// The selection is a plain index into the source model: it stays valid
// across this model's own resets (the source tree is untouched by those),
// and it is cleared synchronously whenever the SOURCE model resets —
// otherwise it would dangle into freed nodes. A persistent index cannot
// work here: one stored mid-reset dies at endResetModel, while one stored
// after leaves proxies with a stale mapping and no refresh signal.
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
    QModelIndex selected_{};
};
