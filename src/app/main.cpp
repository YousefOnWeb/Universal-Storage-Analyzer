#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QTextStream>

#include "../models/FileSystemModel.h"
#include "../services/DatabaseManager.h"
#include "../services/ScannerService.h"
#include "../services/ScanTypes.h"
#include "../ui/MainWindow.h"
#include "../ui/ThemeManager.h"

namespace {

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
        
        QObject::connect(&scanner, &ScannerService::scanFinished, &app, [&app, &out](const QString&, quint64 files, quint64 dirs, quint64 bytes) {
            out << "Scan finished. Files: " << files << " Dirs: " << dirs << " Bytes: " << bytes << "\n";
            out.flush();
            app.quit();
        });
        
        QObject::connect(&scanner, &ScannerService::errorOccurred, &app, [&out](const QString&, const QString& err) {
            out << "Error: " << err << "\n";
            out.flush();
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
