#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QTextStream>

#include <QDateTime>

#include "../models/FileSystemModel.h"
#include "../services/DatabaseManager.h"
#include "../services/ScannerService.h"
#include "../services/ScanTypes.h"
#include "../ui/MainWindow.h"
#include "../ui/ThemeManager.h"

static QFile* g_sessionLogFile = nullptr;
// Intentionally leaked: the handler can fire during static teardown,
// after function-local statics would have been destroyed.
QMutex* sessionLogMutex()
{
    static auto* mutex = new QMutex{};
    return mutex;
}

void sessionLogHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Engine worker threads and the GUI thread all log through here;
    // QFile is reentrant, not thread-safe, so serialize every access.
    const QMutexLocker lock{sessionLogMutex()};
    if (!g_sessionLogFile) return;
    Q_UNUSED(context);
    
    QString timestamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
        + QStringLiteral("Z");
    QString level;
    switch (type) {
    case QtDebugMsg:    level = "DBG"; break;
    case QtInfoMsg:     level = "INF"; break;
    case QtWarningMsg:  level = "WRN"; break;
    case QtCriticalMsg: level = "ERR"; break;
    case QtFatalMsg:    level = "FTL"; break;
    }
    
    QString logLine = QString("[%1] %2: %3\n").arg(timestamp, level, msg);
    g_sessionLogFile->write(logLine.toUtf8());
    g_sessionLogFile->flush();
}

namespace {

// Owns the session debug log for the whole lifetime of main(), including
// every early-return CLI path. Manual open/close leaked the handle and
// dropped the closing line whenever main() returned early.
struct SessionLogGuard {
    SessionLogGuard()
    {
        const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(cfgDir + QStringLiteral("/session-logs"));
        const QString logName = cfgDir + QStringLiteral("/session-logs/session-")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))
            + QStringLiteral(".log");
        g_sessionLogFile = new QFile(logName);
        if (g_sessionLogFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            qInstallMessageHandler(sessionLogHandler);
            qInfo() << "Session debug log started. App Version:"
                    << QCoreApplication::applicationVersion();
        } else {
            delete g_sessionLogFile;
            g_sessionLogFile = nullptr;
        }
    }
    ~SessionLogGuard()
    {
        if (g_sessionLogFile != nullptr) {
            qInfo() << "Session debug log ended. Application closing.";
            g_sessionLogFile->close();
            delete g_sessionLogFile;
            g_sessionLogFile = nullptr;
        }
    }
    SessionLogGuard(const SessionLogGuard&) = delete;
    SessionLogGuard& operator=(const SessionLogGuard&) = delete;
};

int runSmokeTest()
{
    // --smoke-test: init essential services, signal readiness, exit cleanly.
    QTextStream out{stdout};
    DatabaseManager db{};
    QString err{};
    if (!db.ensureReady(&err)) {
        out << "SMOKE-FAIL db: " << err << "\n";
        return 1;
    }
    FileSystemModel model{};
    ScannerService scanner{};
    Q_UNUSED(model);
    Q_UNUSED(scanner);
    out << "SMOKE-OK UniversalStorageAnalyzer ready\n";
    return 0;
}

int runHeadlessCheck()
{
    // --headless-check: validate config, DB, migrations, resources.
    QTextStream out{stdout};
    bool ok{true};
    DatabaseManager db{};
    QString err{};
    if (!db.ensureReady(&err)) {
        out << "CHECK-FAIL db: " << err << "\n";
        ok = false;
    } else {
        out << "CHECK-OK db\n";
    }
    for (const QString& r : {QStringLiteral(":/styles/light.qss"),
                             QStringLiteral(":/styles/dark.qss")}) {
        QFile f{r};
        if (!f.exists()) {
            out << "CHECK-FAIL missing resource: " << r << "\n";
            ok = false;
        } else {
            out << "CHECK-OK resource: " << r << "\n";
        }
    }
    const QString cfg =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (cfg.isEmpty() || !QDir().mkpath(cfg)) {
        out << "CHECK-FAIL app-data dir\n";
        ok = false;
    } else {
        out << "CHECK-OK app-data: " << cfg << "\n";
    }
    return ok ? 0 : 2;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UniversalStorageAnalyzer"));
    QApplication::setApplicationName(QStringLiteral("UniversalStorageAnalyzer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    SessionLogGuard sessionLog{};

    qRegisterMetaType<FileRecord>("FileRecord");
    qRegisterMetaType<QVector<FileRecord>>("QVector<FileRecord>");
    qRegisterMetaType<BranchMetrics>("BranchMetrics");

    QCommandLineParser parser{};
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeOpt{QStringLiteral("smoke-test"),
                                QStringLiteral("Init services and exit 0 when ready.")};
    QCommandLineOption headlessOpt{QStringLiteral("headless-check"),
                                   QStringLiteral("Validate config/db/resources.")};
    QCommandLineOption resetOpt{
        QStringLiteral("reset-test-data"),
        QStringLiteral("Clear test database (test builds / UNIVERSAL_STORAGE_ANALYZER_TEST=1 only).")};
    QCommandLineOption cliScanOpt{
        QStringLiteral("cli-scan"),
        QStringLiteral("Run scan in CLI mode without GUI."),
        QStringLiteral("folder")};
    QCommandLineOption noLogOpt{
        QStringLiteral("no-log"),
        QStringLiteral("Disable writing scan diagnostic log files.")};

    parser.addOptions({smokeOpt, headlessOpt, resetOpt, cliScanOpt, noLogOpt});
    parser.addPositionalArgument(QStringLiteral("target"),
                                 QStringLiteral("Folder to scan on launch."),
                                 QStringLiteral("[folder]"));
    parser.process(app);

    if (parser.isSet(resetOpt)) {
        if (qEnvironmentVariableIsSet("UNIVERSAL_STORAGE_ANALYZER_TEST")) {
            QSqlDatabase::removeDatabase(QStringLiteral("usa-settings"));
            const QString base =
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QFile::remove(base + QStringLiteral("/universal-storage-analyzer.db"));
            QTextStream{stdout} << "Test data reset.\n";
            return 0;
        }
        QTextStream{stderr} << "--reset-test-data requires "
                               "UNIVERSAL_STORAGE_ANALYZER_TEST=1\n";
        return 3;
    }

    DatabaseManager db{};
    QString dbErr{};
    if (!db.ensureReady(&dbErr)) {
        QTextStream{stderr} << "Database init failed: " << dbErr << "\n";
        return 4;
    }

    if (parser.isSet(smokeOpt)) {
        return runSmokeTest();
    }
    if (parser.isSet(headlessOpt)) {
        return runHeadlessCheck();
    }

    if (parser.isSet(cliScanOpt)) {
        app.setQuitOnLastWindowClosed(false);
        ScannerService scanner{};
        QString target = parser.value(cliScanOpt);
        bool enableLog = !parser.isSet(noLogOpt);
        
        QTextStream out(stdout);
        out << "Starting CLI scan for: " << target << "\n";
        if (enableLog) out << "Diagnostic logging is enabled.\n";
        else out << "Diagnostic logging is disabled.\n";
        
        QObject::connect(&scanner, &ScannerService::scanProgress, &app, [&out](const QString&, const QString& path) {
            out << "Scanning: " << path << "\n";
            out.flush();
        });
        
        QObject::connect(&scanner, &ScannerService::scanFinished, &app, [&app](const QString&, quint64 files, quint64 dirs, quint64 bytes) {
            qInfo() << "CLI Scan finished. Files:" << files << "Dirs:" << dirs << "Bytes:" << bytes;
            app.quit();
        });
        
        QObject::connect(&scanner, &ScannerService::errorOccurred, &app, [](const QString&, const QString& err, bool fatal) {
            qCritical() << (fatal ? "CLI Fatal Error:" : "CLI Error:") << err;
        });

        scanner.startScan(target, enableLog);
        return app.exec();
    }

    ThemeManager themes{&app};
    FileSystemModel model{};
    ScannerService scanner{};

    // Async bridging: queued connections keep the GUI thread responsive.
    QObject::connect(&scanner, &ScannerService::nodesDiscovered, &model,
                     &FileSystemModel::onNodesDiscovered, Qt::QueuedConnection);
    QObject::connect(&scanner, &ScannerService::branchMetricsUpdated, &model,
                     &FileSystemModel::onBranchMetricsUpdated, Qt::QueuedConnection);
    QObject::connect(&scanner, &ScannerService::scanStarted, &model,
                     &FileSystemModel::onScanStarted, Qt::QueuedConnection);

    MainWindow win{&model, &scanner, &themes};
    win.show();
    const int rc = app.exec();
    return rc;
}
