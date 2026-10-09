#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <QVector>

// Qt-friendly projection of an engine node for queued signal transport.
// The engine structs stay Qt-free; conversion happens at the boundary.
struct FileRecord {
    quint32 id{0};
    quint32 parentId{0};
    QString name{};
    quint64 logicalSize{0};
    quint64 allocatedSize{0};
    quint64 lastModifiedTs{0};
    bool isDirectory{false};
    bool isCompressed{false};
};

Q_DECLARE_METATYPE(FileRecord)

struct BranchMetrics {
    quint32 nodeId{0};
    quint64 logicalSize{0};
    quint64 allocatedSize{0};
    quint32 fileCount{0};
    quint32 folderCount{0};
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
