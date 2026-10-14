#include "TreeFilterProxy.h"

TreeFilterProxyModel::TreeFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setRecursiveFilteringEnabled(true);
}

void TreeFilterProxyModel::setSearchText(const QString& text)
{
    if (text == search_) {
        return;
    }
    search_ = text;
    invalidateFilter();
}

bool TreeFilterProxyModel::filterAcceptsRow(int sourceRow,
                                            const QModelIndex& sourceParent) const
{
    const QAbstractItemModel* src = sourceModel();
    if (src == nullptr) {
        return false;
    }
    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);
    if (!idx.isValid()) {
        return false;
    }
    const QString name = src->data(idx, Qt::DisplayRole).toString();
    if (!search_.isEmpty()
        && !name.contains(search_, Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}
