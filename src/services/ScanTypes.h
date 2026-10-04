#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

// Qt-friendly projection of usa::domain::FSNode for queued signal transport.
// The domain struct itself stays Qt-free; conversion happens at the boundary.
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
