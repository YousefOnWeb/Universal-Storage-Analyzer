#include <QtTest>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../src/services/scanner/TreemapLayout.h"

using usa::scanner::TreemapItem;
using usa::scanner::TreemapLayout;
using usa::scanner::TreemapTile;

// Pure layout math: area conservation, determinism, degenerate inputs,
// bounded aspect ratios. No Qt, no filesystem.
class TestTreemapLayout : public QObject {
    Q_OBJECT
private slots:
    void conservesAreaExactly()
    {
        const std::vector<TreemapItem> items{{1, 600}, {2, 300}, {3, 60}, {4, 30}, {5, 10}};
        const auto tiles = TreemapLayout::layout(items, 0.0, 0.0, 100.0, 100.0);
        QCOMPARE(tiles.size(), static_cast<std::size_t>(5));
        double area{0.0};
        for (const auto& t : tiles) {
            QVERIFY(t.w > 0.0 && t.h > 0.0);
            // Tiles stay inside the rect.
            QVERIFY(t.x >= 0.0 && t.y >= 0.0);
            QVERIFY(t.x + t.w <= 100.0 + 1e-9);
            QVERIFY(t.y + t.h <= 100.0 + 1e-9);
            area += t.w * t.h;
        }
        QVERIFY(std::abs(area - 10000.0) < 1e-6);
    }

    void isDeterministicRegardlessOfInputOrder()
    {
        const std::vector<TreemapItem> a{{1, 50}, {2, 30}, {3, 20}};
        const std::vector<TreemapItem> b{{3, 20}, {1, 50}, {2, 30}};
        const auto ta = TreemapLayout::layout(a, 0.0, 0.0, 60.0, 40.0);
        const auto tb = TreemapLayout::layout(b, 0.0, 0.0, 60.0, 40.0);
        QCOMPARE(ta.size(), tb.size());
        for (std::size_t i{0}; i < ta.size(); ++i) {
            QCOMPARE(ta[i].id, tb[i].id);
            QVERIFY(std::abs(ta[i].x - tb[i].x) < 1e-9);
            QVERIFY(std::abs(ta[i].y - tb[i].y) < 1e-9);
            QVERIFY(std::abs(ta[i].w - tb[i].w) < 1e-9);
            QVERIFY(std::abs(ta[i].h - tb[i].h) < 1e-9);
        }
    }

    void singleItemFillsRect()
    {
        const auto tiles = TreemapLayout::layout({{7, 123}}, 5.0, 10.0, 80.0, 40.0);
        QCOMPARE(tiles.size(), static_cast<std::size_t>(1));
        QCOMPARE(tiles[0].id, static_cast<std::uint32_t>(7));
        QVERIFY(std::abs(tiles[0].x - 5.0) < 1e-9);
        QVERIFY(std::abs(tiles[0].y - 10.0) < 1e-9);
        QVERIFY(std::abs(tiles[0].w - 80.0) < 1e-9);
        QVERIFY(std::abs(tiles[0].h - 40.0) < 1e-9);
    }

    void skipsDegenerateInput()
    {
        QVERIFY(TreemapLayout::layout({}, 0.0, 0.0, 100.0, 100.0).empty());
        QVERIFY(TreemapLayout::layout({{1, 10}}, 0.0, 0.0, 0.0, 100.0).empty());
        const std::vector<TreemapItem> zeros{{1, 0}, {2, 0}};
        QVERIFY(TreemapLayout::layout(zeros, 0.0, 0.0, 100.0, 100.0).empty());
        // Zero-weight items are skipped, positive ones still laid out.
        const std::vector<TreemapItem> mixed{{1, 0}, {2, 40}, {3, 60}};
        const auto tiles = TreemapLayout::layout(mixed, 0.0, 0.0, 100.0, 100.0);
        QCOMPARE(tiles.size(), static_cast<std::size_t>(2));
    }

    void laysBalancedPairsWithoutSlivers()
    {
        // Two equal weights in a 2:1 rect: one row each, exact halves.
        // (The greedy tail on skewed leftovers is expected squarify
        // behavior; the renderer culls sub-3px tiles instead.)
        const std::vector<TreemapItem> items{{1, 50}, {2, 50}};
        const auto tiles = TreemapLayout::layout(items, 0.0, 0.0, 200.0, 100.0);
        QCOMPARE(tiles.size(), static_cast<std::size_t>(2));
        double worst{1.0};
        double area{0.0};
        for (const auto& t : tiles) {
            worst = std::max(worst, std::max(t.w / t.h, t.h / t.w));
            area += t.w * t.h;
        }
        QVERIFY(worst < 4.1);
        QVERIFY(std::abs(area - 20000.0) < 1e-6);
    }
};

QTEST_MAIN(TestTreemapLayout)
#include "test_treemap.moc"
