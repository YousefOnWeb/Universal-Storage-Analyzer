#include "StatsTableModel.h"

#include "../models/FileSystemModel.h"

StatsTableModel::StatsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void StatsTableModel::setHeaders(const QStringList& headers)
{
    beginResetModel();
    headers_ = headers;
    rows_.clear();
    endResetModel();
}

void StatsTableModel::setRows(QVector<StatsRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

int StatsTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int StatsTableModel::columnCount(const QModelIndex& /*parent*/) const
{
    return headers_.size();
}

QVariant StatsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const StatsRow& row = rows_.at(index.row());
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        return (index.column() >= 0 && index.column() < row.text.size())
                   ? QVariant(row.text.at(index.column()))
                   : QVariant{};
    }
    if (role == FileSystemModel::Roles::SortRole) {
        return (index.column() >= 0 && index.column() < row.keys.size())
                   ? QVariant(row.keys.at(index.column()))
                   : QVariant{};
    }
    if (role == Qt::UserRole) {
        return row.nodeId;
    }
    return {};
}

QVariant StatsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    return (section >= 0 && section < headers_.size()) ? QVariant(headers_.at(section))
                                                       : QVariant{};
}

quint32 StatsTableModel::rowNodeId(int row) const
{
    return (row >= 0 && row < rows_.size()) ? rows_.at(row).nodeId : 0;
}
