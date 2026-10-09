#pragma once

#include <QLocale>
#include <QPalette>
#include <QString>
#include <QWidget>

#include <cstdint>

// Shared presentation helpers for the analytics charts (bars, pie,
// treemap): stable per-id hues and locale-aware byte strings. Pure QtGui,
// no model or filesystem access.
namespace usa::ui {

// Stable spread of ids across the hue wheel (Knuth multiplicative hash),
// so a node keeps its color across recomputes and panes.
inline int chartHue(std::uint32_t id)
{
    return static_cast<int>((static_cast<std::uint64_t>(id) * 2654435761ULL) % 360ULL);
}

inline bool lightTheme(const QWidget* widget)
{
    return widget->palette().color(QPalette::Window).lightness() > 128;
}

inline QColor chartColor(std::uint32_t id, bool strong, const QWidget* widget)
{
    QColor color{};
    color.setHsv(chartHue(id), strong ? 150 : 90, lightTheme(widget) ? 225 : 165);
    return color;
}

inline QString chartBytes(std::uint64_t bytes)
{
    return QLocale{}.toString(bytes) + QStringLiteral(" bytes");
}

} // namespace usa::ui
