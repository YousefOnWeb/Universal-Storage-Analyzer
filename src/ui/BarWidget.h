#pragma once

#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../models/FileSystemModel.h"
#include "../models/SizeFormat.h"

// Horizontal bar chart of the selected folder's largest children: quick
// capacity comparison with exact values. Custom-painted (no QtCharts
// dependency); data comes from the in-memory model only, so it is instant
// on any scan. Click a bar to select the node in the tree.
class BarWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxBars{20};

    explicit BarWidget(FileSystemModel* model, QWidget* parent = nullptr);

    quint32 rootId() const { return rootId_; }
    int barCount() const { return static_cast<int>(bars_.size()); } // test hook
    QString sizeUnit() const { return sizeUnit_; } // test hook

public slots:
    void setRootId(quint32 nodeId);
    void refreshSoon();
    void selectRow(int row);
    // Spec 006b: unit for value labels and tooltips (default Auto).
    void setSizeUnit(const QString& unit);

signals:
    void barClicked(quint32 nodeId);
    void barDoubleClicked(quint32 nodeId);

protected:
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Bar {
        quint32 id{0};
        QString name{};
        quint64 bytes{0};
        bool isDir{false};
        QColor color{};
    };

    static constexpr int kRowH{26};
    static constexpr int kPad{8};
    static constexpr int kNameW{200};
    static constexpr int kValueW{130};

    void recompute();
    int rowAt(int y) const;

    FileSystemModel* model_{nullptr};
    quint32 rootId_{0};
    QString sizeUnit_{QStringLiteral("Auto")};
    std::vector<Bar> bars_{};
    std::uint64_t max_{0};
    int hover_{-1};
    QTimer debounce_{};
};
