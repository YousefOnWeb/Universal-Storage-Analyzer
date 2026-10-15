#pragma once

#include <QAbstractItemModel>
#include <QSet>
#include <QTimer>
#include <QVector>

#include "../services/ScanTypes.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Virtualized tree model: QTreeView renders only visible rows while the
// scanner streams batches in the background. All mutations arrive via
// queued slots on the GUI thread — never touched from the worker thread.
class FileSystemModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Columns : int {
        ColName = 0,
        ColSize = 1,
        ColAllocated = 2,
        ColFiles = 3,
        ColFolders = 4,
        ColSharePct = 5,
        ColModified = 6,
        ColCount = 7,
    };

    enum Roles : int {
        SortRole = Qt::UserRole + 1,
    };

    explicit FileSystemModel(QObject* parent = nullptr);

    // QAbstractItemModel
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    quint32 nodeIdForIndex(const QModelIndex& index) const;
    QModelIndex indexForNodeId(quint32 nodeId) const;
    quint64 totalLogicalBytes() const { return totalLogical_; }
    // Spec 006b: unit for size DisplayRole strings ("Auto", "KB",
    // "MB", "GB"). SortRole keys and stored values never change —
    // switching re-renders visible strings only.
    void setSizeUnit(const QString& unit);
    // Accumulated GUI-thread mutation wall time (inserts + summary
    // applies), for scan-speed diagnosis. Reset per scan.
    quint64 insertUsec() const { return insertUs_; }

    // Treemap/analytics support: structural reads for layout input.
    struct ChildEntry {
        quint32 id{0};
        QString name{};
        quint64 logicalSize{0};
        bool isDirectory{false};
    };
    std::vector<ChildEntry> childEntries(quint32 nodeId) const;
    bool isDirectoryNode(quint32 nodeId) const;
    std::uint64_t lastModifiedTs(quint32 nodeId) const;

public slots:
    void clear();
    void onNodesDiscovered(const QString& taskId, QVector<FileRecord> batch);
    void onBranchMetricsUpdated(const QString& taskId, BranchMetrics metrics);
    void onSweepUpdated(const QString& taskId, const QVector<BranchMetrics>& summaries);
    void onScanStarted(const QString& taskId, const QString& targetPath);

signals:
    void modelResetForNewScan(const QString& targetPath);

public:
    // Reserved id for the synthetic root-files group row (spec 005c).
    // Engine ids sequence from 1 and can never reach it; records
    // carrying it are dropped defensively on insert.
    static constexpr quint32 kRootFilesGroupId{0xFFFFFFFE};

private:
    struct Node {
        quint32 id{0};
        quint32 parentId{0};
        // Source row within the parent. Fixed at insert time (children are
        // only ever appended), so index lookup stays O(1) no matter how
        // wide a directory gets.
        int row{-1};
        QString name{};
        quint64 logicalSize{0};
        quint64 allocatedSize{0};
        quint32 fileCount{0};
        quint32 folderCount{0};
        quint64 lastModifiedTs{0};
        bool isDirectory{false};
        Node* parent{nullptr};
        QVector<Node*> children{};
    };

    Node* nodeFromIndex(const QModelIndex& index) const;
    void applyMetrics(Node* node, const BranchMetrics& metrics);
    // Spec 006a: share of the parent's logical total. Returns false (no
    // value, not zero) for the root and for zero parent totals; callers
    // render the em dash / sort below real shares from that.
    static bool sharePercent(const Node* node, double* pctOut);
    void refreshNode(const QModelIndex& top, Node* node);
    // Inserts one batch grouped into contiguous same-parent transactions.
    // Newly inserted ids drain any waiting children (recursive, sequential:
    // each group opens and closes its own rows-insert transaction).
    void insertGrouped(const QVector<FileRecord>& batch);
    // Inserts [start, start + count) of batch beneath parentNode in one
    // rows-insert transaction; appends fresh ids for drain duty.
    void insertRowsUnder(Node* parentNode, const QVector<FileRecord>& batch, int start, int count,
                         std::vector<quint32>& freshIds);
    // Drains children that waited for freshly inserted parents.
    void drainWaitingChildren(const std::vector<quint32>& freshIds);
    // Spec 005c: ensures the synthetic "[N Files]" group row beneath the
    // scan root (created on first root-level file, never otherwise).
    Node* ensureRootFilesGroup();
    // Spec 005c: appends root-level files as group members (no
    // individual tree rows) and rolls their sizes into the group sums.
    void insertRootFileMembers(const QVector<FileRecord>& files);

    std::unordered_map<quint32, std::unique_ptr<Node>> owned_{};
    // Summaries that arrived before their node (node batches are chunked
    // for signal efficiency while summaries stream immediately). Applied
    // on insert; latest wins.
    std::unordered_map<quint32, BranchMetrics> pendingSummaries_{};
    // Nodes whose parent has not arrived yet (MFT/traversal order scatters
    // families across batches and workers, so child-before-parent is
    // routine). Drained when the parent inserts — never misattached to
    // root. Cleared on reset; a correct engine always delivers the parent.
    std::unordered_map<quint32, QVector<FileRecord>> pendingChildren_;
    // View-notification coalescing: values are stored immediately, but
    // dataChanged (which drives proxy re-filtering and repaints) fires at
    // most every 100 ms, bounded per tick so burst catch-up (e.g. the
    // end-of-scan sweep) amortizes instead of stalling the event loop.
    // The bound must stay generous: at 100/tick a 330k sweep would need
    // minutes to drain; at 2000 it settles in seconds with ~20 ms of work
    // per tick, keeping the loop responsive throughout.
    // Without both bounds, hundreds of thousands of summary signals per
    // scan would keep the event loop from ever idling.
    static constexpr int kMaxEmissionsPerTick{2000};
    QSet<quint32> dirtyIds_{};
    QTimer coalesceTimer_{};
    Node* root_{nullptr};
    quint64 totalLogical_{0};
    std::uint64_t insertUs_{0};
    // Display unit for size strings; persisted separately (main/units).
    QString sizeUnit_{QStringLiteral("Auto")};

private slots:
    void flushDirtySummaries();
};
