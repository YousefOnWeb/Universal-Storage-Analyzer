#include "TreemapWidget.h"

#include <QFontMetrics>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QStringList>
#include <QToolTip>

#include <algorithm>
#include <unordered_map>

#include "ChartUtils.h"

#include "../services/scanner/TreemapLayout.h"

namespace {

constexpr double kCullPx{3.0};
constexpr int kDebounceMs{250};

} // namespace

TreemapWidget::TreemapWidget(FileSystemModel* model, QWidget* parent)
    : QWidget(parent)
    , model_(model)
{
    setMouseTracking(true);
    setMinimumHeight(120);
    setAccessibleName(QStringLiteral("Storage treemap"));
    setToolTip(QStringLiteral("Tile area is proportional to size; click to select"));
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &TreemapWidget::recompute);
}

void TreemapWidget::setRootId(quint32 nodeId)
{
    if (nodeId == rootId_) {
        return;
    }
    rootId_ = nodeId;
    hover_ = -1;
    recompute();
}

void TreemapWidget::refreshSoon()
{
    if (!debounce_.isActive()) {
        debounce_.start();
    }
}

void TreemapWidget::selectTile(quint32 nodeId)
{
    for (const auto& t : tiles_) {
        if (t.id == nodeId) {
            emit tileClicked(nodeId);
            return;
        }
    }
}

void TreemapWidget::recompute()
{
    debounce_.stop();
    tiles_.clear();
    total_ = 0;
    hover_ = -1;
    if (model_ != nullptr && width() > 0 && height() > 0) {
        auto entries = model_->childEntries(rootId_);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.logicalSize > b.logicalSize;
        });
        std::vector<usa::scanner::TreemapItem> items{};
        items.reserve(entries.size());
        for (const auto& e : entries) {
            items.push_back({e.id, static_cast<double>(e.logicalSize)});
            total_ += e.logicalSize;
        }
        const auto laid =
            usa::scanner::TreemapLayout::layout(items, 0.0, 0.0,
                                               static_cast<double>(width()),
                                               static_cast<double>(height()));
        std::unordered_map<quint32, const FileSystemModel::ChildEntry*> byId{};
        byId.reserve(entries.size());
        for (const auto& e : entries) {
            byId.emplace(e.id, &e);
        }
        tiles_.reserve(laid.size());
        for (const auto& lt : laid) {
            const auto it = byId.find(lt.id);
            if (it == byId.end()) {
                continue;
            }
            const auto* e = it->second;
            Tile t{};
            t.id = lt.id;
            t.rect = QRectF(lt.x, lt.y, lt.w, lt.h);
            t.name = e->name;
            t.bytes = e->logicalSize;
            t.isDir = e->isDirectory;
            QColor c = usa::ui::chartColor(t.id, t.isDir, this);
            t.color = c;
            tiles_.push_back(std::move(t));
        }
    }
    // Render once into the cache; paints blit it.
    const qreal dpr = devicePixelRatioF();
    cachedFor_ = size();
    cache_ = QPixmap(static_cast<int>(static_cast<double>(width()) * dpr),
                     static_cast<int>(static_cast<double>(height()) * dpr));
    cache_.setDevicePixelRatio(dpr);
    cache_.fill(palette().color(QPalette::Window));
    if (!tiles_.empty()) {
        QPainter p{&cache_};
        QFontMetrics fm{font()};
        for (const auto& t : tiles_) {
            if (t.rect.width() < kCullPx || t.rect.height() < kCullPx) {
                continue;
            }
            p.fillRect(t.rect, t.color);
            p.setPen(t.color.darker(140));
            p.drawRect(t.rect);
            if (t.rect.height() >= 16.0 && t.rect.width() >= 40.0) {
                p.setPen(t.color.lightness() > 128 ? Qt::black : Qt::white);
                const QString label = fm.elidedText(t.name, Qt::ElideRight,
                                                    static_cast<int>(t.rect.width()) - 4);
                p.drawText(t.rect.adjusted(2, 2, -2, -2),
                           static_cast<int>(Qt::AlignLeft | Qt::AlignTop), label);
            }
        }
    } else {
        QPainter p{&cache_};
        p.setPen(palette().color(QPalette::WindowText));
        QString hint{};
        if (model_ == nullptr || model_->rowCount(QModelIndex{}) == 0) {
            hint = QStringLiteral("No data yet — start a scan");
        } else {
            hint = QStringLiteral("Empty folder");
        }
        p.drawText(rect(), static_cast<int>(Qt::AlignCenter), hint);
    }
    update();
}

int TreemapWidget::tileAt(const QPointF& pos) const
{
    for (int i = static_cast<int>(tiles_.size()) - 1; i >= 0; --i) {
        const auto& t = tiles_[i];
        if (t.rect.width() < kCullPx || t.rect.height() < kCullPx) {
            continue;
        }
        if (t.rect.contains(pos)) {
            return i;
        }
    }
    return -1;
}

QString TreemapWidget::nodePath(quint32 nodeId) const
{
    if (model_ == nullptr) {
        return {};
    }
    QStringList parts{};
    quint32 id = nodeId;
    for (int depth{0}; depth < 1024; ++depth) {
        const QModelIndex idx = model_->indexForNodeId(id);
        if (!idx.isValid()) {
            break;
        }
        parts.prepend(model_->data(model_->index(idx.row(), FileSystemModel::ColName,
                                                              idx.parent()))
                          .toString());
        if (id == rootId_) {
            break;
        }
        // Walk up via the proxy-free parent chain.
        const QModelIndex p = model_->parent(idx);
        if (!p.isValid()) {
            break;
        }
        id = model_->nodeIdForIndex(p);
    }
    return parts.join(QStringLiteral("/"));
}

void TreemapWidget::paintEvent(QPaintEvent* event)
{
    QPainter p{this};
    if (cachedFor_ != size() && width() > 0 && height() > 0) {
        // Stale cache (missed resize path): rebuild synchronously once.
        recompute();
    }
    if (!cache_.isNull()) {
        p.drawPixmap(0, 0, cache_);
    }
    if (hover_ >= 0 && hover_ < static_cast<int>(tiles_.size())) {
        const auto& t = tiles_[static_cast<std::size_t>(hover_)];
        p.setPen(QPen(palette().color(QPalette::Highlight), 2));
        p.drawRect(t.rect);
    }
    QWidget::paintEvent(event);
}

void TreemapWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    recompute();
}

void TreemapWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int idx = tileAt(event->position());
    if (idx != hover_) {
        hover_ = idx;
        update();
    }
    if (idx >= 0) {
        const auto& t = tiles_[static_cast<std::size_t>(idx)];
        const double share = total_ > 0 ? (static_cast<double>(t.bytes) * 100.0
                                          / static_cast<double>(total_))
                                        : 0.0;
        QToolTip::showText(
            mapToGlobal(event->position().toPoint()),
            QStringLiteral("%1\n%2 (%3%)")
                .arg(nodePath(t.id), usa::ui::chartBytes(t.bytes),
                     QString::number(share, 'f', 1)),
            this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void TreemapWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = tileAt(event->position());
        if (idx >= 0) {
            selectTile(tiles_[static_cast<std::size_t>(idx)].id);
        }
    }
    QWidget::mousePressEvent(event);
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = tileAt(event->position());
        if (idx >= 0) {
            emit tileDoubleClicked(tiles_[static_cast<std::size_t>(idx)].id);
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TreemapWidget::leaveEvent(QEvent* event)
{
    hover_ = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}
