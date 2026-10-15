#pragma once

#include <QString>

#include <cstdint>

// Unit-aware size rendering shared by the tree/details models, the
// charts, and the main window (spec 006b). Sort keys always stay raw
// integers; only display strings pass through here.
namespace usa::models {

// Renders bytes in the requested unit ("Auto", "KB", "MB", "GB").
// Auto buckets B → KB → MB → GB → TB by magnitude (1024-based).
// Anything else renders raw bytes with the unit as suffix.
// Two decimals, always.
inline QString formatSizeBytes(std::uint64_t bytes, const QString& unit)
{
    double v{static_cast<double>(bytes)};
    QString suffix{unit};
    if (unit == QStringLiteral("Auto")) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int idx{0};
        while (v >= 1024.0 && idx < 4) {
            v /= 1024.0;
            ++idx;
        }
        suffix = QString::fromUtf8(units[idx]);
    } else {
        if (unit == QStringLiteral("KB")) {
            v /= 1024.0;
        } else if (unit == QStringLiteral("MB")) {
            v /= 1024.0 * 1024.0;
        } else if (unit == QStringLiteral("GB")) {
            v /= 1024.0 * 1024.0 * 1024.0;
        }
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 2).arg(suffix);
}

// Exact byte count for tooltips and copy paths ("123456789 bytes").
inline QString exactSizeBytes(std::uint64_t bytes)
{
    return QString::number(bytes) + QStringLiteral(" bytes");
}

} // namespace usa::models
