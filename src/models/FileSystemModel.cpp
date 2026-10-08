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
    coalesceTimer_.setSingleShot(true);
    coalesceTimer_.setInterval(100);
    connect(&coalesceTimer_, &QTimer::timeout, this, &FileSystemModel::flushDirtySummaries);
}

void FileSystemModel::flushDirtySummaries()
{
    for (const quint32 id : std::as_const(dirtyIds_)) {
        const auto it = owned_.find(id);
        if (it != owned_.end()) {
            refreshNode(indexForNodeId(id), it->second.get());
        }
    }
    dirtyIds_.clear();
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
    return createIndex(p->row, 0, const_cast<Node*>(p));
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
    Node* node = it->second.get();
    return createIndex(node->row, 0, node);
}

void FileSystemModel::clear()
{
    beginResetModel();
    owned_.clear();
    pendingSummaries_.clear();
    dirtyIds_.clear();
    coalesceTimer_.stop();
    dirtyIds_.clear();
    coalesceTimer_.stop();
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
    pendingSummaries_.clear();
    dirtyIds_.clear();
    coalesceTimer_.stop();
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
        node->row = row;
        node->name = rec.name;
        node->logicalSize = rec.logicalSize;
        node->allocatedSize = rec.allocatedSize;
        node->lastModifiedTs = rec.lastModifiedTs;
        node->isDirectory = rec.isDirectory;
        node->parent = parentNode;
        Node* raw = node.get();
        owned_.emplace(raw->id, std::move(node));
        parentNode->children.push_back(raw);

        // Aggregation lives in the scan engine: it streams absolute
        // per-directory totals via branchMetricsUpdated(), which this model
        // stores verbatim. Per-file upward propagation here would count
        // every byte twice, so nodes are stored exactly as delivered.
        if (!raw->isDirectory) {
            totalLogical_ += raw->logicalSize;
        }

        // A summary may have arrived before its node (see
        // onBranchMetricsUpdated); apply it now so nothing is lost.
        if (const auto pending = pendingSummaries_.find(raw->id);
            pending != pendingSummaries_.end()) {
            applyMetrics(raw, pending->second);
            pendingSummaries_.erase(pending);
        }

        endInsertRows();
    }
}

void FileSystemModel::onBranchMetricsUpdated(const QString& /*taskId*/, BranchMetrics metrics)
{
    const auto it = owned_.find(metrics.nodeId);
    if (it == owned_.end()) {
        // Node batches are chunked while summaries stream immediately, so a
        // summary can precede its node. Stash it; the insert path applies it.
        pendingSummaries_[metrics.nodeId] = metrics;
        return;
    }
    // Replace semantics: the engine reports absolute running totals per
    // directory (already propagated up the ancestor chain engine-side), so
    // the model stores them verbatim. Adding here would double-count.
    // View notification is coalesced (see flushDirtySummaries): values are
    // current immediately, repaint/proxy work happens at most every 100 ms.
    Node* node = it->second.get();
    applyMetrics(node, metrics);
    dirtyIds_.insert(node->id);
    if (!coalesceTimer_.isActive()) {
        coalesceTimer_.start();
    }
}

void FileSystemModel::applyMetrics(Node* node, const BranchMetrics& metrics)
{
    node->logicalSize = metrics.logicalSize;
    node->allocatedSize = metrics.allocatedSize;
    node->fileCount = metrics.fileCount;
    node->folderCount = metrics.folderCount;
}

void FileSystemModel::refreshNode(const QModelIndex& top, Node* node)
{
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
