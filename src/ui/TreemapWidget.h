#pragma once

#include <QPixmap>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../models/FileSystemModel.h"

// Squarified treemap of the selected subtree's children: each tile's area
// is proportional to logical bytes, so the pane always reconciles with
// the tree. Layout math lives in scanner/TreemapLayout (Qt-free); this
// class only renders and routes interaction.
//
// Rendering is retained: tiles render once into a pixmap cache on
// selection/model/resize changes; paint events blit the cache plus a
// hover overlay. Sub-3px tiles are culled at paint and hit-test time.
// Live model churn funnels through refreshSoon() (250 ms debounce), so
// scans cannot repaint-storm the GUI thread.
class TreemapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TreemapWidget(FileSystemModel* model, QWidget* parent = nullptr);

    quint32 rootId() const { return rootId_; }
    std::size_t tileCount() const { return tiles_.size(); } // test hook

public slots:
    void setRootId(quint32 nodeId);
    void refreshSoon();
    void selectTile(quint32 nodeId);

signals:
    void tileClicked(quint32 nodeId);
    void tileDoubleClicked(quint32 nodeId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Tile {
        quint32 id{0};
        QRectF rect{};
        QString name{};
        quint64 bytes{0};
        bool isDir{false};
        QColor color{};
    };

    void recompute();
    int tileAt(const QPointF& pos) const;
    QString nodePath(quint32 nodeId) const;

    FileSystemModel* model_{nullptr};
    quint32 rootId_{0};
    std::vector<Tile> tiles_{};
    std::uint64_t total_{0};
    QPixmap cache_{};
    QSize cachedFor_{};
    int hover_{-1};
    QTimer debounce_{};
};
