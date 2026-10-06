#include "DatabaseManager.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

DatabaseManager::DatabaseManager(QObject* parent)
    : QObject(parent)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    dbPath_ = base + QStringLiteral("/universal-storage-analyzer.db");
}

bool DatabaseManager::ensureReady(QString* error)
{
    // Re-entrant: runSmokeTest() constructs a second manager in the same
    // process, so drop any previous registration instead of shadowing it
    // (which Qt reports as "duplicate connection name").
    if (QSqlDatabase::contains(QStringLiteral("usa-settings"))) {
        QSqlDatabase::removeDatabase(QStringLiteral("usa-settings"));
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                QStringLiteral("usa-settings"));
    db.setDatabaseName(dbPath_);
    if (!db.open()) {
        if (error) {
            *error = db.lastError().text();
        }
        return false;
    }
    if (!applyMigrationV1(error)) {
        return false;
    }
    ready_ = true;
    return true;
}

bool DatabaseManager::applyMigrationV1(QString* error)
{
    // Migration: V1__Initial_Schema (see resources/migrations/V1__Initial_Schema.sql).
    // Executed idempotently with CREATE TABLE IF NOT EXISTS.
    QSqlDatabase db = QSqlDatabase::database(QStringLiteral("usa-settings"));
    QSqlQuery q{db};
    const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS AppSettings ("
        " key_name VARCHAR(100) PRIMARY KEY,"
        " value_data TEXT NOT NULL,"
        " updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)",
        "CREATE TABLE IF NOT EXISTS ScanSnapshots ("
        " snapshot_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " target_path VARCHAR(1024) NOT NULL,"
        " total_logical_bytes BIGINT NOT NULL,"
        " total_allocated_bytes BIGINT NOT NULL,"
        " scan_date DATETIME DEFAULT CURRENT_TIMESTAMP,"
        " index_file_path VARCHAR(1024) NOT NULL)",
        "CREATE TABLE IF NOT EXISTS ScheduledTasks ("
        " task_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " target_path VARCHAR(1024) NOT NULL,"
        " export_format VARCHAR(10) NOT NULL,"
        " cron_schedule VARCHAR(50) NOT NULL,"
        " is_active BOOLEAN DEFAULT 1)",
    };
    for (const char* stmt : ddl) {
        if (!q.exec(QString::fromUtf8(stmt))) {
            if (error) {
                *error = q.lastError().text();
            }
            return false;
        }
    }
    const char* seeds[][2] = {
        {"theme_mode", "system"},
        {"age_intervals_days", "1,7,30,180,365"},
    };
    for (const auto& seed : seeds) {
        q.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO AppSettings (key_name, value_data) VALUES (?, ?)"));
        q.addBindValue(QString::fromUtf8(seed[0]));
        q.addBindValue(QString::fromUtf8(seed[1]));
        if (!q.exec() && error) {
            *error = q.lastError().text();
            return false;
        }
    }
    return true;
}
