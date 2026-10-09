#pragma once

#include <QStorageInfo>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../models/FileSystemModel.h"

// Pie chart of storage distribution for the scan target's volume: the
// selected folder's largest children, an "Other files" remainder, and
// free space. Custom-painted (no QtCharts dependency); volume figures
// come from QStorageInfo, per-node figures from the in-memory model.
// Click a node slice to select it in the tree; legend carries values,
// never color-only.
class PieWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxSlices{8};
    static constexpr quint32 kNoId{0xFFFFFFFFu};

    explicit PieWidget(FileSystemModel* model, QWidget* parent = nullptr);

    quint32 rootId() const { return rootId_; }
    int sliceCount() const { return static_cast<int>(slices_.size()); } // test hook
    quint32 sliceNodeId(int i) const; // kNoId for Other/Free slices

public slots:
    void setRootId(quint32 nodeId);
    void setTargetPath(const QString& path);
    void refreshSoon();
    void selectSlice(int i);

signals:
    void pieClicked(quint32 nodeId);
    void pieDoubleClicked(quint32 nodeId);

protected:
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Slice {
        quint32 id{kNoId};
        QString label{};
        quint64 bytes{0};
        QColor color{};
    };

    static constexpr int kDebounceMs{250};
    static constexpr double kMinAngleDeg{2.0};

    void recompute();
    int sliceAt(const QPointF& pos) const;
    double startAngle(int i) const; // degrees, 12 o'clock clockwise

    FileSystemModel* model_{nullptr};
    quint32 rootId_{0};
    QString targetPath_{};
    std::vector<Slice> slices_{};
    std::uint64_t total_{0};
    int hover_{-1};
    QTimer debounce_{};
};
