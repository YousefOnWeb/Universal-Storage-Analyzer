#include "FileSystemModel.h"

#include <QDateTime>

FileSystemModel::FileSystemModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    auto root = std::make_unique<Node>();
    root->id = 0;
    root->parentId = 0;
    root->name = QStringLiteral("/");
    root->isDirectory = true;
    root_ = root.get();
    owned_.emplace(0, std::move(root));
}

QModelIndex FileSystemModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) {
        return {};
    }
    const Node* parentNode = parent.isValid() ? nodeFromIndex(parent) : root_;
    if (parentNode == nullptr || row < 0
        || row >= static_cast<int>(parentNode->children.size())) {
        return {};
    }
    Node* child = parentNode->children.at(static_cast<std::size_t>(row));
    return createIndex(row, column, child);
}

QModelIndex FileSystemModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return {};
    }
    const Node* node = nodeFromIndex(child);
    if (node == nullptr || node->parent == nullptr || node->parent == root_) {
        return {};
    }
    const Node* p = node->parent;
    const Node* gp = p->parent != nullptr ? p->parent : root_;
    int row{0};
    for (int i{0}; i < static_cast<int>(gp->children.size()); ++i) {
        if (gp->children.at(static_cast<std::size_t>(i)) == p) {
            row = i;
            break;
        }
    }
    return createIndex(row, 0, const_cast<Node*>(p));
}

int FileSystemModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0) {
        return 0;
    }
    const Node* node = parent.isValid() ? nodeFromIndex(parent) : root_;
    if (node == nullptr) {
        return 0;
    }
    return static_cast<int>(node->children.size());
}

int FileSystemModel::columnCount(const QModelIndex& /*parent*/) const
{
    return ColCount;
}

QVariant FileSystemModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const Node* node = nodeFromIndex(index);
    if (node == nullptr) {
        return {};
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case ColName:
            return node->name;
        case ColSize:
            return static_cast<quint64>(node->logicalSize);
        case ColAllocated:
            return static_cast<quint64>(node->allocatedSize);
        case ColFiles:
            return node->isDirectory ? QVariant(node->fileCount) : QVariant{};
        case ColFolders:
            return node->isDirectory ? QVariant(node->folderCount) : QVariant{};
        case ColModified:
            if (node->lastModifiedTs == 0) {
                return {};
            }
            return QDateTime::fromSecsSinceEpoch(
                       static_cast<qint64>(node->lastModifiedTs))
                .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        default:
            return {};
        }
    }
    if (role == Qt::UserRole) {
        return node->id;
    }
    return {};
}

QVariant FileSystemModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case ColName:
        return QStringLiteral("Name");
    case ColSize:
        return QStringLiteral("Size");
    case ColAllocated:
        return QStringLiteral("Allocated");
    case ColFiles:
        return QStringLiteral("Files");
    case ColFolders:
        return QStringLiteral("Folders");
    case ColModified:
        return QStringLiteral("Last Modified");
    default:
        return {};
    }
}

quint32 FileSystemModel::nodeIdForIndex(const QModelIndex& index) const
{
    const Node* node = nodeFromIndex(index);
    return node != nullptr ? node->id : 0;
}

QModelIndex FileSystemModel::indexForNodeId(quint32 nodeId) const
{
    const auto it = owned_.find(nodeId);
    if (it == owned_.end() || it->second.get() == root_) {
        return {};
    }
    const Node* node = it->second.get();
    const Node* p = node->parent != nullptr ? node->parent : root_;
    int row{0};
    for (int i{0}; i < static_cast<int>(p->children.size()); ++i) {
        if (p->children.at(static_cast<std::size_t>(i)) == node) {
            row = i;
            break;
        }
    }
    return createIndex(row, 0, const_cast<Node*>(node));
}

void FileSystemModel::clear()
{
    beginResetModel();
    owned_.clear();
    auto root = std::make_unique<Node>();
    root->id = 0;
    root->parentId = 0;
    root->name = QStringLiteral("/");
    root->isDirectory = true;
    root_ = root.get();
    owned_.emplace(0, std::move(root));
    totalLogical_ = 0;
    endResetModel();
}

void FileSystemModel::onScanStarted(const QString& /*taskId*/, const QString& targetPath)
{
    beginResetModel();
    owned_.clear();
    auto root = std::make_unique<Node>();
    root->id = 0;
    root->parentId = 0;
    root->name = targetPath;
    root->isDirectory = true;
    root_ = root.get();
    owned_.emplace(0, std::move(root));
    totalLogical_ = 0;
    endResetModel();
    emit modelResetForNewScan(targetPath);
}

void FileSystemModel::onNodesDiscovered(const QString& /*taskId*/, QVector<FileRecord> batch)
{
    for (const FileRecord& rec : batch) {
        if (rec.id == 0) {
            continue; // root already exists
        }
        if (owned_.find(rec.id) != owned_.end()) {
            continue;
        }
        const auto parentIt = owned_.find(rec.parentId);
        Node* parentNode = parentIt != owned_.end() ? parentIt->second.get() : root_;
        if (parentNode == nullptr) {
            parentNode = root_;
        }

        const QModelIndex parentIdx =
            (parentNode == root_) ? QModelIndex{} : indexForNodeId(parentNode->id);
        const int row = static_cast<int>(parentNode->children.size());
        beginInsertRows(parentIdx, row, row);

        auto node = std::make_unique<Node>();
        node->id = rec.id;
        node->parentId = parentNode->id;
        node->name = rec.name;
        node->logicalSize = rec.logicalSize;
        node->allocatedSize = rec.allocatedSize;
        node->lastModifiedTs = rec.lastModifiedTs;
        node->isDirectory = rec.isDirectory;
        node->parent = parentNode;
        Node* raw = node.get();
        owned_.emplace(raw->id, std::move(node));
        parentNode->children.push_back(raw);

        // Live aggregation: files contribute size upward immediately so the
        // tree shows growing totals while the scan runs.
        if (!raw->isDirectory) {
            propagateToAncestors(parentNode, raw->logicalSize, raw->allocatedSize, true,
                                 false);
            totalLogical_ += raw->logicalSize;
        } else {
            propagateToAncestors(parentNode, 0, 0, false, true);
        }

        endInsertRows();
    }
}

void FileSystemModel::onBranchMetricsUpdated(const QString& /*taskId*/, BranchMetrics metrics)
{
    const auto it = owned_.find(metrics.nodeId);
    if (it == owned_.end()) {
        return;
    }
    Node* node = it->second.get();
    node->logicalSize += metrics.logicalSize;
    node->allocatedSize += metrics.allocatedSize;
    node->fileCount += metrics.fileCount;
    node->folderCount += metrics.folderCount;
    if (node->parent != nullptr && node != root_) {
        propagateToAncestors(node->parent, metrics.logicalSize, metrics.allocatedSize, false,
                             false);
        // Counts were already propagated per-node on discovery; branch totals
        // here only carry sizes to avoid double counting.
    } else if (node == root_) {
        totalLogical_ = node->logicalSize;
    }
    const QModelIndex top = indexForNodeId(node->id);
    if (top.isValid()) {
        const QModelIndex bottom =
            createIndex(top.row(), ColCount - 1, static_cast<void*>(node));
        emit dataChanged(top, bottom, {Qt::DisplayRole});
    }
}

FileSystemModel::Node* FileSystemModel::nodeFromIndex(const QModelIndex& index) const
{
    return static_cast<Node*>(index.internalPointer());
}

void FileSystemModel::propagateToAncestors(Node* start, quint64 logicalDelta,
                                           quint64 allocatedDelta, bool countFile,
                                           bool countFolder)
{
    for (Node* cur{start}; cur != nullptr; cur = cur->parent) {
        cur->logicalSize += logicalDelta;
        cur->allocatedSize += allocatedDelta;
        if (countFile) {
            ++cur->fileCount;
        }
        if (countFolder) {
            ++cur->folderCount;
        }
        if (cur == root_) {
            break;
        }
    }
}
