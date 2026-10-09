#include "ScanLogger.h"
#include "scanner/ScanEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QString>
#include <QSysInfo>
#include <QTextStream>

static QString formatBytes(qint64 bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return QStringLiteral("%1 bytes (%2 GiB)").arg(bytes).arg(gb, 0, 'f', 2);
}

static QString formatBytes(quint64 bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return QStringLiteral("%1 bytes (%2 GiB)").arg(bytes).arg(gb, 0, 'f', 2);
}

// Task IDs become filename components: strip anything that could escape
// the scan-logs directory on any platform.
static QString sanitizeForFilename(const QString& taskId)
{
    QString safe{taskId};
    for (auto& ch : safe) {
        const ushort u = ch.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                        || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.';
        if (!ok) {
            ch = u'_';
        }
    }
    return safe.isEmpty() ? QStringLiteral("scan") : safe;
}

void ScanLogger::writeLog(const QString& taskId, const QString& targetPath, const usa::scanner::EngineResult& result)
{
    const QString logsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/scan-logs");
    QDir().mkpath(logsDir);

    const QDateTime now = QDateTime::currentDateTime();
    const QString timestamp = now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString pid = QString::number(QCoreApplication::applicationPid());
    const QString filename =
        QStringLiteral("scan_%1_pid%2_%3.log").arg(timestamp, pid, sanitizeForFilename(taskId));
    const QString filePath = logsDir + QStringLiteral("/") + filename;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "ScanLogger: cannot write diagnostic log" << filePath << file.errorString();
        return;
    }

    QTextStream out(&file);

    out << "================================================================================\n";
    out << "Universal Storage Analyzer - scan diagnostic log\n";
    out << "Task:         " << taskId << "\n";
    out << "Log created:  " << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << "\n";
    out << "--------------------------------------------------------------------------------\n";

    QFileInfo targetInfo(targetPath);
    out << "[Target]\n";
    out << "Path:         " << targetPath << "\n";
    out << "Exists:       " << (targetInfo.exists() ? "yes" : "no") << "\n";
    QString kind = "unknown";
    if (targetInfo.exists()) {
        if (targetInfo.isDir()) kind = "directory";
        else if (targetInfo.isFile()) kind = "file";
        else if (targetInfo.isSymLink()) kind = "symlink";
    }
    out << "Kind:         " << kind << "\n";

    QStorageInfo storage(targetPath);
    out << "[Volume]\n";
    out << "Root:         " << storage.rootPath() << "\n";
    out << "Device:       " << storage.device() << "\n";
    out << "Display name: " << storage.displayName() << "\n";
    out << "File system:  " << storage.fileSystemType() << "\n";
    out << "Total:        " << formatBytes(storage.bytesTotal()) << "\n";
    out << "Free:         " << formatBytes(storage.bytesFree()) << "\n";
    out << "Available:    " << formatBytes(storage.bytesAvailable()) << "\n";
    out << "Block size:   " << storage.blockSize() << "\n";
    out << "Read-only:    " << (storage.isReadOnly() ? "yes" : "no") << "\n";

    out << "[Host]\n";
    out << "OS:           " << QSysInfo::prettyProductName() << "\n";
    out << "Kernel:       " << QSysInfo::kernelType() << " " << QSysInfo::kernelVersion() << "\n";
    out << "CPU arch:     " << QSysInfo::currentCpuArchitecture() << "\n";
    out << "Qt runtime:   " << qVersion() << "\n";

    out << "[Engine]\n";
    QString backendStr = "portable std::filesystem (iterative DFS)";
    if (result.backend == usa::scanner::BackendKind::DirectMft) {
        backendStr = "Direct NTFS Master File Table (MFT) parser";
    }
    out << "Traversal:    " << backendStr << "\n";
    out << "Batching:     engine " << usa::scanner::ScanEngine::kBatchNodes
        << " records/batch, UI re-chunk 500 per nodesDiscovered event\n";
    out << "Allocated:    backend-reported allocation (NTFS AllocationSize /\n";
    out << "              POSIX st_blocks), never a fixed cluster estimate\n";
    out << "Threading:    multi-threaded engine pool, serialized callbacks;\n";
    out << "              Qt bridge on dedicated QThread, queued delivery\n";

    out << "--------------------------------------------------------------------------------\n";
    out << "[Result]\n";
    out << "Finished:     " << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << "\n";
    out << "Duration:     " << result.durationMs << " ms\n";
    out << "Files:        " << result.fileCount << "\n";
    out << "Directories:  " << result.dirCount << "\n";
    out << "Total bytes:  " << formatBytes(result.totalBytes) << "\n";
    out << "Batches:      " << result.batchCount << " (" << (result.fileCount + result.dirCount) << " records)\n";
    out << "Errors:       " << result.errorCount << "\n";

    double mb = static_cast<double>(result.totalBytes) / (1024.0 * 1024.0);
    double sec = static_cast<double>(result.durationMs) / 1000.0;
    double throughput = sec > 0.0 ? (mb / sec) : 0.0;
    out << "Throughput:   " << QString::number(throughput, 'f', 2) << " MiB/s (logical bytes / duration)\n";

    out << "================================================================================\n";

    out.flush();
    file.close();
}
