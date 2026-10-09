#include "TreeFilterProxy.h"

TreeFilterProxyModel::TreeFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setRecursiveFilteringEnabled(true);
}

void TreeFilterProxyModel::setSourceModel(QAbstractItemModel* source)
{
    QSortFilterProxyModel::setSourceModel(source);
    files_ = qobject_cast<FileSystemModel*>(source);
}

void TreeFilterProxyModel::setSearchText(const QString& text)
{
    if (text == search_) {
        return;
    }
    search_ = text;
    invalidateFilter();
}

void TreeFilterProxyModel::setExtensionFilter(const QString& ext)
{
    if (ext == ext_) {
        return;
    }
    ext_ = ext;
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
    if (!ext_.isEmpty() && files_ != nullptr) {
        // Containers pass on search alone; the recursive filter keeps
        // ancestor chains of matching files visible regardless.
        const bool isDir = files_->isDirectoryNode(files_->nodeIdForIndex(idx));
        if (!isDir && !name.endsWith(ext_, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}
