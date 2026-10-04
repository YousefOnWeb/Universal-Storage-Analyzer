#include "LocalScannerWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

quint64 toUnixTs(const std::filesystem::file_time_type& ft)
{
    const auto sysTime = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::file_clock::to_sys(ft));
    return static_cast<quint64>(sysTime.time_since_epoch().count());
}

quint64 alignedSize(quint64 logical)
{
    if (logical == 0) {
        return 0;
    }
    const quint64 cluster{LocalScannerWorker::kClusterSize};
    return ((logical + cluster - 1) / cluster) * cluster;
}

} // namespace

LocalScannerWorker::LocalScannerWorker(QObject* parent)
    : QObject(parent)
{
}

void LocalScannerWorker::requestPause(bool paused)
{
    paused_.store(paused, std::memory_order_relaxed);
}

void LocalScannerWorker::requestCancel()
{
    cancelled_.store(true, std::memory_order_relaxed);
}

void LocalScannerWorker::startScanPath(const QString& taskId, const QString& targetPath)
{
    paused_.store(false, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);

    emit scanStarted(taskId, targetPath);

    const std::filesystem::path root{targetPath.toStdWString()};
    std::error_code ec{};
    if (!std::filesystem::exists(root, ec) || ec) {
        emit errorOccurred(taskId, QStringLiteral("Target path does not exist: %1").arg(targetPath));
        emit scanFinished(taskId, 0, 0, 0);
        return;
    }

    // Synthetic ids: backend owns ids; UI mirrors them verbatim.
    quint32 nextId{1};
    const quint32 rootId{0};

    {
        // Emit the root node first so the tree has an anchor.
        FileRecord rec{};
        rec.id = rootId;
        rec.parentId = rootId;
        rec.name = targetPath;
        rec.isDirectory = true;
        emit nodesDiscovered(taskId, QVector<FileRecord>{rec});
    }

    struct Frame {
        std::filesystem::path path;
        quint32 id;
        quint64 logical{0};
        quint64 allocated{0};
        quint32 files{0};
        quint32 folders{0};
    };

    QVector<FileRecord> batch{};
    batch.reserve(kBatchSize);

    auto flush = [&]() {
        if (!batch.isEmpty()) {
            emit nodesDiscovered(taskId, batch);
            batch.clear();
            batch.reserve(kBatchSize);
        }
    };

    quint64 totalFiles{0};
    quint64 totalDirs{0};
    quint64 totalBytes{0};
    int progressTick{0};

    // Iterative DFS keeps stack memory bounded on deep hierarchies and
    // supports pause/cancel checks without recursion unwind cost.
    std::vector<Frame> stack{};
    stack.push_back(Frame{root, rootId});

    while (!stack.empty()) {
        if (cancelled_.load(std::memory_order_relaxed)) {
            flush();
            emit scanFinished(taskId, totalFiles, totalDirs, totalBytes);
            return;
        }
        while (paused_.load(std::memory_order_relaxed)) {
            if (cancelled_.load(std::memory_order_relaxed)) {
                flush();
                emit scanFinished(taskId, totalFiles, totalDirs, totalBytes);
                return;
            }
            QThread::msleep(50);
        }

        Frame frame = std::move(stack.back());
        stack.pop_back();

        std::error_code iterEc{};
        std::filesystem::directory_iterator it{frame.path, iterEc};
        if (iterEc) {
            continue; // permission denied etc. — skip, do not abort scan
        }

        const std::filesystem::directory_iterator end{};
        for (; it != end; it.increment(iterEc)) {
            if (iterEc) {
                continue;
            }
            if (cancelled_.load(std::memory_order_relaxed)) {
                break;
            }
            while (paused_.load(std::memory_order_relaxed)
                   && !cancelled_.load(std::memory_order_relaxed)) {
                QThread::msleep(50);
            }

            const auto& entryPath = it->path();
            std::error_code typeEc{};
            const bool entryIsDir = it->is_directory(typeEc) && !typeEc;

            std::error_code sizeEc{};
            std::error_code timeEc{};
            quint64 logical{0};
            quint64 mtime{0};
            if (!entryIsDir) {
                const auto sz = it->file_size(sizeEc);
                if (!sizeEc) {
                    logical = static_cast<quint64>(sz);
                }
                const auto ft = std::filesystem::last_write_time(entryPath, timeEc);
                if (!timeEc) {
                    mtime = toUnixTs(ft);
                }
            } else {
                const auto ft = std::filesystem::last_write_time(entryPath, timeEc);
                if (!timeEc) {
                    mtime = toUnixTs(ft);
                }
            }

            const quint32 id{nextId++};
            const QString qname =
                QString::fromStdWString(entryPath.filename().wstring());

            FileRecord rec{};
            rec.id = id;
            rec.parentId = frame.id;
            rec.name = qname.isEmpty() ? QString::fromStdWString(entryPath.wstring()) : qname;
            rec.logicalSize = logical;
            rec.allocatedSize = entryIsDir ? 0 : alignedSize(logical);
            rec.lastModifiedTs = mtime;
            rec.isDirectory = entryIsDir;
            batch.push_back(rec);

            if (entryIsDir) {
                ++frame.folders;
                ++totalDirs;
                stack.push_back(Frame{entryPath, id});
            } else {
                ++frame.files;
                ++totalFiles;
                totalBytes += logical;
                frame.logical += logical;
                frame.allocated += rec.allocatedSize;
            }

            if (static_cast<int>(batch.size()) >= kBatchSize) {
                flush();
            }
            if ((++progressTick % 2000) == 0) {
                emit scanProgress(taskId, QString::fromStdWString(entryPath.wstring()));
            }
        }

        flush();
        BranchMetrics m{};
        m.nodeId = frame.id;
        m.logicalSize = frame.logical;
        m.allocatedSize = frame.allocated;
        m.fileCount = frame.files;
        m.folderCount = frame.folders;
        emit branchMetricsUpdated(taskId, m);

        // Bubble this directory's totals into its parent frame if still on stack.
        // Because frames are value-popped, propagate via a metrics chain:
        // the model accumulates ancestors itself; here we just continue.
        Q_UNUSED(rootId);
    }

    flush();
    emit scanFinished(taskId, totalFiles, totalDirs, totalBytes);
}
