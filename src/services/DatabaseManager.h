#pragma once

#include <QObject>
#include <QString>

// Ensures the SQLite store exists and V1 migration is applied.
// Lives in the GUI thread; all queries are short-lived settings/snapshot ops.
class DatabaseManager : public QObject {
    Q_OBJECT
public:
    explicit DatabaseManager(QObject* parent = nullptr);

    bool ensureReady(QString* error = nullptr);
    bool isReady() const { return ready_; }

private:
    bool applyMigrationV1(QString* error);
    QString dbPath_;
    bool ready_{false};
};
