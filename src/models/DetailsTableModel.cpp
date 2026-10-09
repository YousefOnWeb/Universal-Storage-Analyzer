#include "DetailsTableModel.h"

DetailsTableModel::DetailsTableModel(FileSystemModel* source, QObject* parent)
    : QAbstractTableModel(parent)
    , source_(source)
{
}

int DetailsTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() || source_ == nullptr || !selected_.isValid()) {
        return 0;
    }
    return source_->rowCount(selected_);
}

int DetailsTableModel::columnCount(const QModelIndex& /*parent*/) const
{
    return FileSystemModel::ColCount;
}

QVariant DetailsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || source_ == nullptr || !selected_.isValid()) {
        return {};
    }
    const QModelIndex src = source_->index(index.row(), index.column(), selected_);
    if (!src.isValid()) {
        return {};
    }
    return source_->data(src, role);
}

QVariant DetailsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (source_ == nullptr) {
        return {};
    }
    return source_->headerData(section, orientation, role);
}

void DetailsTableModel::setSelectedNode(const QModelIndex& sourceIndex)
{
    beginResetModel();
    selected_ = sourceIndex;
    endResetModel();
}
