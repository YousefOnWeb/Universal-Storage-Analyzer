#include "BarWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>

#include <algorithm>

#include "ChartUtils.h"

namespace {

constexpr int kDebounceMs{250};

} // namespace

BarWidget::BarWidget(FileSystemModel* model, QWidget* parent)
    : QWidget(parent)
    , model_(model)
{
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Directory bar chart"));
    setToolTip(QStringLiteral("Top children by size; click a bar to select"));
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &BarWidget::recompute);
}

void BarWidget::setRootId(quint32 nodeId)
{
    if (nodeId == rootId_) {
        return;
    }
    rootId_ = nodeId;
    hover_ = -1;
    recompute();
}

void BarWidget::refreshSoon()
{
    if (!debounce_.isActive()) {
        debounce_.start();
    }
}

void BarWidget::setSizeUnit(const QString& unit)
{
    if (unit == sizeUnit_) {
        return;
    }
    sizeUnit_ = unit;
    update(); // labels/tooltips re-render from stored raw bytes
}

void BarWidget::selectRow(int row)
{
    if (row >= 0 && row < static_cast<int>(bars_.size())) {
        emit barClicked(bars_[static_cast<std::size_t>(row)].id);
    }
}

QSize BarWidget::sizeHint() const
{
    const int rows = std::max<int>(static_cast<int>(bars_.size()), 1);
    return {kNameW + kValueW + 3 * kPad + 200, rows * kRowH + 2 * kPad};
}

void BarWidget::recompute()
{
    debounce_.stop();
    bars_.clear();
    max_ = 0;
    hover_ = -1;
    if (model_ != nullptr) {
        auto entries = model_->childEntries(rootId_);
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.logicalSize > b.logicalSize; });
        const std::size_t n = std::min<std::size_t>(entries.size(), kMaxBars);
        bars_.reserve(n);
        for (std::size_t i{0}; i < n; ++i) {
            const auto& e = entries[i];
            Bar b{};
            b.id = e.id;
            b.name = e.name;
            b.bytes = e.logicalSize;
            b.isDir = e.isDirectory;
            b.color = usa::ui::chartColor(e.id, e.isDirectory, this);
            bars_.push_back(std::move(b));
            max_ = std::max(max_, e.logicalSize);
        }
    }
    updateGeometry();
    update();
}

int BarWidget::rowAt(int y) const
{
    const int row = (y - kPad) / kRowH;
    if (row < 0 || row >= static_cast<int>(bars_.size())) {
        return -1;
    }
    return row;
}

void BarWidget::paintEvent(QPaintEvent* event)
{
    QPainter p{this};
    p.fillRect(rect(), palette().color(QPalette::Window));
    if (bars_.empty()) {
        p.setPen(palette().color(QPalette::WindowText));
        QString hint{};
        if (model_ == nullptr || model_->rowCount(QModelIndex{}) == 0) {
            hint = QStringLiteral("No data yet — start a scan");
        } else {
            hint = QStringLiteral("Nothing to show");
        }
        p.drawText(rect(), static_cast<int>(Qt::AlignCenter), hint);
        return;
    }
    QFontMetrics fm{font()};
    const int barX = kPad + kNameW;
    const int barW = std::max(0, width() - barX - kValueW - 2 * kPad);
    for (int i{0}; i < static_cast<int>(bars_.size()); ++i) {
        const auto& b = bars_[static_cast<std::size_t>(i)];
        const int y = kPad + i * kRowH;
        if (i == hover_) {
            p.fillRect(0, y - 2, width(), kRowH, palette().color(QPalette::Highlight));
        }
        p.setPen(palette().color(QPalette::WindowText));
        const QString name = fm.elidedText(b.name, Qt::ElideRight, kNameW - 4);
        p.drawText(kPad, y, kNameW - 4, kRowH - 6,
                   static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter), name);
        const int fill = (max_ > 0 && barW > 0)
                             ? static_cast<int>(static_cast<double>(b.bytes)
                                                / static_cast<double>(max_)
                                                * static_cast<double>(barW))
                             : 0;
        const QRect barRect{barX, y + 3, fill, kRowH - 9};
        p.fillRect(barRect, b.color);
        p.setPen(b.color.darker(140));
        p.drawRect(barX, y + 3, std::max(fill, 1), kRowH - 9);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(barX + barW + kPad, y, kValueW - kPad, kRowH - 6,
                   static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                   usa::models::formatSizeBytes(b.bytes, sizeUnit_));
    }
    QWidget::paintEvent(event);
}

void BarWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int row = rowAt(static_cast<int>(event->position().y()));
    if (row != hover_) {
        hover_ = row;
        update();
    }
    if (row >= 0) {
        const auto& b = bars_[static_cast<std::size_t>(row)];
        QToolTip::showText(mapToGlobal(event->position().toPoint()),
                           QStringLiteral("%1\n%2").arg(
                               b.name, usa::models::formatSizeBytes(b.bytes, sizeUnit_)),
                           this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void BarWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        selectRow(rowAt(static_cast<int>(event->position().y())));
    }
    QWidget::mousePressEvent(event);
}

void BarWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int row = rowAt(static_cast<int>(event->position().y()));
        if (row >= 0) {
            emit barDoubleClicked(bars_[static_cast<std::size_t>(row)].id);
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void BarWidget::leaveEvent(QEvent* event)
{
    hover_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}
