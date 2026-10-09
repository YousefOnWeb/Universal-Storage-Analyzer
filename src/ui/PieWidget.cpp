#include "PieWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>

#include <algorithm>
#include <cmath>

#include "ChartUtils.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

PieWidget::PieWidget(FileSystemModel* model, QWidget* parent)
    : QWidget(parent)
    , model_(model)
{
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Storage pie chart"));
    setToolTip(QStringLiteral("Volume distribution; click a slice to select"));
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &PieWidget::recompute);
}

quint32 PieWidget::sliceNodeId(int i) const
{
    if (i < 0 || i >= static_cast<int>(slices_.size())) {
        return kNoId;
    }
    return slices_[static_cast<std::size_t>(i)].id;
}

void PieWidget::setRootId(quint32 nodeId)
{
    if (nodeId == rootId_) {
        return;
    }
    rootId_ = nodeId;
    hover_ = -1;
    recompute();
}

void PieWidget::setTargetPath(const QString& path)
{
    if (path == targetPath_) {
        return;
    }
    targetPath_ = path;
    hover_ = -1;
    recompute();
}

void PieWidget::refreshSoon()
{
    if (!debounce_.isActive()) {
        debounce_.start();
    }
}

void PieWidget::selectSlice(int i)
{
    if (i >= 0 && i < static_cast<int>(slices_.size())
        && slices_[static_cast<std::size_t>(i)].id != kNoId) {
        emit pieClicked(slices_[static_cast<std::size_t>(i)].id);
    }
}

QSize PieWidget::sizeHint() const
{
    return {420, 280};
}

void PieWidget::recompute()
{
    debounce_.stop();
    slices_.clear();
    total_ = 0;
    hover_ = -1;
    QStorageInfo storage{targetPath_};
    if (model_ == nullptr || !storage.isValid() || storage.bytesTotal() <= 0) {
        update();
        return;
    }
    const auto total = static_cast<std::uint64_t>(storage.bytesTotal());
    const auto free = static_cast<std::uint64_t>(storage.bytesAvailable());
    const std::uint64_t used = total > free ? total - free : 0;
    total_ = total;

    auto entries = model_->childEntries(rootId_);
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.logicalSize > b.logicalSize; });
    std::uint64_t childSum{0};
    const std::size_t n = std::min<std::size_t>(entries.size(), kMaxSlices);
    for (std::size_t i{0}; i < n; ++i) {
        const auto& e = entries[i];
        Slice s{};
        s.id = e.id;
        s.label = e.name;
        s.bytes = e.logicalSize;
        s.color = usa::ui::chartColor(e.id, e.isDirectory, this);
        slices_.push_back(std::move(s));
        childSum += e.logicalSize;
    }
    if (used > childSum) {
        Slice other{};
        other.label = QStringLiteral("Other files");
        other.bytes = used - childSum;
        other.color = palette().color(QPalette::Mid);
        slices_.push_back(std::move(other));
    }
    if (free > 0) {
        Slice fs{};
        fs.label = QStringLiteral("Free space");
        fs.bytes = free;
        fs.color = usa::ui::lightTheme(this) ? QColor{156, 204, 156} : QColor{58, 95, 58};
        slices_.push_back(std::move(fs));
    }
    update();
}

double PieWidget::startAngle(int i) const
{
    // 12 o'clock, clockwise, in degrees (QPainter's pie convention with a
    // -90 offset handled at draw/hit time).
    double frac{0.0};
    for (int k = 0; k < i; ++k) {
        if (total_ > 0) {
            frac += static_cast<double>(slices_[static_cast<std::size_t>(k)].bytes)
                    / static_cast<double>(total_);
        }
    }
    return frac * 360.0;
}

int PieWidget::sliceAt(const QPointF& pos) const
{
    if (slices_.empty() || total_ == 0) {
        return -1;
    }
    const double diameter =
        std::min(static_cast<double>(width() - 220), static_cast<double>(height() - 16));
    if (diameter <= 0.0) {
        return -1;
    }
    const QPointF center{8.0 + diameter / 2.0, 8.0 + diameter / 2.0};
    const QPointF d = pos - center;
    const double dist = std::hypot(d.x(), d.y());
    if (dist > diameter / 2.0) {
        return -1;
    }
    double angle = std::atan2(d.y(), d.x()) * 180.0 / kPi + 90.0; // 0 at top, clockwise
    if (angle < 0.0) {
        angle += 360.0;
    }
    const double frac = angle / 360.0;
    double acc{0.0};
    for (int i = 0; i < static_cast<int>(slices_.size()); ++i) {
        const auto& s = slices_[static_cast<std::size_t>(i)];
        const double span = static_cast<double>(s.bytes) / static_cast<double>(total_);
        if (frac >= acc && frac < acc + span) {
            return (span * 360.0 >= kMinAngleDeg) ? i : -1;
        }
        acc += span;
    }
    return -1;
}

void PieWidget::paintEvent(QPaintEvent* event)
{
    QPainter p{this};
    p.fillRect(rect(), palette().color(QPalette::Window));
    if (slices_.empty() || total_ == 0) {
        p.setPen(palette().color(QPalette::WindowText));
        QString hint{};
        if (model_ == nullptr || model_->rowCount(QModelIndex{}) == 0) {
            hint = QStringLiteral("No data yet — start a scan");
        } else {
            hint = QStringLiteral("Volume unavailable for the target path");
        }
        p.drawText(rect(), static_cast<int>(Qt::AlignCenter), hint);
        return;
    }
    const double diameter =
        std::min(static_cast<double>(width() - 220), static_cast<double>(height() - 16));
    const QRectF pie{8.0, 8.0, diameter, diameter};
    QFontMetrics fm{font()};
    int legendY{12};
    for (int i = 0; i < static_cast<int>(slices_.size()); ++i) {
        const auto& s = slices_[static_cast<std::size_t>(i)];
        const double span = static_cast<double>(s.bytes) / static_cast<double>(total_) * 360.0;
        if (span >= kMinAngleDeg) {
            // QPainter: 0 deg at 3 o'clock, counter-clockwise positive.
            p.setBrush(s.color);
            p.setPen(i == hover_ ? QPen(palette().color(QPalette::Highlight), 2)
                                 : QPen(s.color.darker(140), 1));
            p.drawPie(pie, static_cast<int>((90.0 - startAngle(i) - span) * 16.0),
                      static_cast<int>(span * 16.0));
        }
        // Legend: swatch + name + value + share (never color-only).
        const double pct =
            static_cast<double>(s.bytes) * 100.0 / static_cast<double>(total_);
        const int lx = static_cast<int>(pie.right()) + 16;
        p.fillRect(lx, legendY, 12, 12, s.color);
        p.setPen(palette().color(QPalette::WindowText));
        const QString text = QStringLiteral("%1 — %2 (%3%)")
                                 .arg(s.label, usa::ui::chartBytes(s.bytes),
                                      QString::number(pct, 'f', 1));
        p.drawText(lx + 18, legendY, width() - lx - 24, 16,
                   static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                   fm.elidedText(text, Qt::ElideRight, width() - lx - 24));
        legendY += 20;
    }
    QWidget::paintEvent(event);
}

void PieWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int idx = sliceAt(event->position());
    if (idx != hover_) {
        hover_ = idx;
        update();
    }
    if (idx >= 0) {
        const auto& s = slices_[static_cast<std::size_t>(idx)];
        const double pct =
            total_ > 0
                ? (static_cast<double>(s.bytes) * 100.0 / static_cast<double>(total_))
                : 0.0;
        QToolTip::showText(
            mapToGlobal(event->position().toPoint()),
            QStringLiteral("%1\n%2 (%3%)")
                .arg(s.label, usa::ui::chartBytes(s.bytes), QString::number(pct, 'f', 1)),
            this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void PieWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        selectSlice(sliceAt(event->position()));
    }
    QWidget::mousePressEvent(event);
}

void PieWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = sliceAt(event->position());
        if (idx >= 0 && slices_[static_cast<std::size_t>(idx)].id != kNoId) {
            emit pieDoubleClicked(slices_[static_cast<std::size_t>(idx)].id);
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PieWidget::leaveEvent(QEvent* event)
{
    hover_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}
