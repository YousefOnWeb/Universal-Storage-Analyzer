#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <cstddef>
#include <cstdint>
#include <vector>

// Qt-friendly projection of an engine node for queued signal transport.
// The engine structs stay Qt-free; conversion happens at the boundary.
struct FileRecord {
    quint32 id{0};
    quint32 parentId{0};
    QString name{};
    quint64 logicalSize{0};
    quint64 allocatedSize{0};
    quint64 lastModifiedTs{0};
    quint64 lastAccessTs{0}; // Unix seconds, 0 when unknown (spec 006d)
    bool isDirectory{false};
    bool isCompressed{false};
    // Backend-opaque owner identity (spec 006c); empty when the backend
    // cannot provide it. Display names resolve lazily in the model.
    std::string ownerId{};
};

Q_DECLARE_METATYPE(FileRecord)

struct BranchMetrics {
    quint32 nodeId{0};
    quint64 logicalSize{0};
    quint64 allocatedSize{0};
    quint32 fileCount{0};
    quint32 folderCount{0};
    // Subtree-max last-write, Unix seconds (spec 006e). Directories
    // display max(object mtime, this); files never carry summaries.
    quint64 maxMtime{0};
};

Q_DECLARE_METATYPE(BranchMetrics)

// Qt-friendly analytics snapshot for queued signal transport. Converted
// from usa::scanner::AnalyticsSnapshot at the bridge; display strings
// (% shares, bucket labels) are built here, never in the engine.
struct AnalyticsExtension {
    QString extension{};
    quint64 bytes{0};
    quint64 files{0};
    double sharePct{0.0};
};

struct AnalyticsTopFile {
    quint32 nodeId{0};
    QString path{};
    QString name{};
    quint64 bytes{0};
};

struct AnalyticsAgeBucket {
    QString label{};
    quint64 files{0};
    quint64 bytes{0};
};

struct AnalyticsOwner {
    QString owner{};
    quint64 bytes{0};
    quint64 files{0};
    double sharePct{0.0};
};

struct AnalyticsData {
    QVector<AnalyticsExtension> extensions{};
    QVector<AnalyticsTopFile> top{};
    QVector<AnalyticsAgeBucket> age{};
    QVector<AnalyticsOwner> owners{};
    quint64 totalBytes{0};
    quint64 totalFiles{0};
};

Q_DECLARE_METATYPE(AnalyticsData)

// Bucket i covers [edges[i-1], edges[i]); the last covers [edges.back(),
// inf). Shared by the worker bridge and the in-memory re-aggregation so
// both label identically.
inline QString ageBucketLabel(std::size_t bucket, const std::vector<std::uint64_t>& edges)
{
    const auto days = [](std::uint64_t d) {
        return d == 1 ? QStringLiteral("1 day") : QString::number(d) + QStringLiteral(" days");
    };
    if (edges.empty()) {
        return QStringLiteral("unknown");
    }
    if (bucket == 0) {
        return QStringLiteral("< ") + days(edges.front());
    }
    if (bucket < edges.size()) {
        return days(edges[bucket - 1]) + QStringLiteral(" – ") + days(edges[bucket]);
    }
    return QStringLiteral("≥ ") + days(edges.back());
}
