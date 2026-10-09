#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace usa::scanner {

// Squarified treemap layout (Bruls et al.): pure geometry, no Qt, no I/O.
// Input is one level — typically the selected subtree's children with
// their byte weights. Output tiles are deterministic for the same input
// and conserve area exactly (sum of tile areas == rect area, up to fp
// rounding), so tile sums always reconcile with tree totals.
struct TreemapItem {
    std::uint32_t id{0};
    double weight{0.0};
};

struct TreemapTile {
    std::uint32_t id{0};
    double x{0.0};
    double y{0.0};
    double w{0.0};
    double h{0.0};
};

class TreemapLayout {
public:
    // Lay out items (sorted internally, descending) into the rect.
    // Zero-weight items are skipped; empty input or a degenerate rect
    // yields no tiles.
    static std::vector<TreemapTile> layout(const std::vector<TreemapItem>& items, double x,
                                           double y, double w, double h)
    {
        std::vector<TreemapTile> out{};
        if (items.empty() || w <= 0.0 || h <= 0.0) {
            return out;
        }
        std::vector<TreemapItem> sorted{};
        sorted.reserve(items.size());
        double total{0.0};
        for (const auto& it : items) {
            if (it.weight > 0.0) {
                sorted.push_back(it);
                total += it.weight;
            }
        }
        if (sorted.empty() || total <= 0.0) {
            return out;
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const TreemapItem& a, const TreemapItem& b) { return a.weight > b.weight; });
        // Normalize to rect area so areas reconcile exactly.
        const double area = w * h;
        for (auto& it : sorted) {
            it.weight = it.weight / total * area;
        }
        out.reserve(sorted.size());

        double rx{x};
        double ry{y};
        double rw{w};
        double rh{h};
        std::vector<TreemapItem> row{};
        row.reserve(sorted.size());
        double rowArea{0.0};
        for (const auto& it : sorted) {
            const double side = rh < rw ? rh : rw;
            if (!row.empty() && worse(row, rowArea, it.weight, side)) {
                layoutRow(row, rowArea, rx, ry, rw, rh, out);
                row.clear();
                rowArea = 0.0;
            }
            row.push_back(it);
            rowArea += it.weight;
        }
        if (!row.empty()) {
            layoutRow(row, rowArea, rx, ry, rw, rh, out);
        }
        return out;
    }

private:
    // Worst aspect ratio in row (weight/area units): max(area/max, min/area).
    static double worst(const std::vector<TreemapItem>& row, double rowArea, double side)
    {
        double maxW{0.0};
        double minW{rowArea};
        for (const auto& it : row) {
            maxW = std::max(maxW, it.weight);
            minW = std::min(minW, it.weight);
        }
        const double s2 = side * side;
        const double a2 = rowArea * rowArea;
        return std::max(s2 * maxW / a2, a2 / (s2 * minW));
    }

    static bool worse(const std::vector<TreemapItem>& row, double rowArea, double next,
                      double side)
    {
        if (rowArea <= 0.0) {
            return false;
        }
        return worst(row, rowArea, side) < worstWith(row, rowArea, next, side);
    }

    static double worstWith(const std::vector<TreemapItem>& row, double rowArea, double next,
                            double side)
    {
        const double total = rowArea + next;
        double maxW = next;
        double minW = next;
        for (const auto& it : row) {
            maxW = std::max(maxW, it.weight);
            minW = std::min(minW, it.weight);
        }
        const double s2 = side * side;
        const double a2 = total * total;
        return std::max(s2 * maxW / a2, a2 / (s2 * minW));
    }

    // Lay one row along the shortest remaining side, then shrink the rect.
    static void layoutRow(const std::vector<TreemapItem>& row, double rowArea, double& rx,
                          double& ry, double& rw, double& rh, std::vector<TreemapTile>& out)
    {
        const bool horizontal = rw >= rh;
        const double thickness = horizontal ? rowArea / rw : rowArea / rh;
        double off{0.0};
        for (const auto& it : row) {
            const double len = it.weight / rowArea
                               * (horizontal ? rw : rh);
            TreemapTile t{};
            t.id = it.id;
            if (horizontal) {
                t.x = rx + off;
                t.y = ry;
                t.w = len;
                t.h = thickness;
            } else {
                t.x = rx;
                t.y = ry + off;
                t.w = thickness;
                t.h = len;
            }
            out.push_back(t);
            off += len;
        }
        if (horizontal) {
            ry += thickness;
            rh -= thickness;
        } else {
            rx += thickness;
            rw -= thickness;
        }
    }
};

} // namespace usa::scanner
