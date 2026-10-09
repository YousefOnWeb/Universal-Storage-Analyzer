#pragma once

#include <QAbstractTableModel>
#include <QStringList>
#include <QVector>

#include <cstdint>

// One generic read-only table for the analytics panes (Extensions, Top
// Files, Users, Age): the bridge replaces all rows per snapshot, views
// sort through the numeric sort role. Display strings (including % and
// formatted sizes) are built at the Qt boundary, never in the engine.
struct StatsRow {
    QStringList text{}; // one display string per column
    QVector<quint64> keys{}; // numeric sort key per column (SortRole)
    quint32 nodeId{0}; // tree selection target, 0 = none
};

class StatsTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit StatsTableModel(QObject* parent = nullptr);

    void setHeaders(const QStringList& headers);
    void setRows(QVector<StatsRow> rows);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    quint32 rowNodeId(int row) const;

private:
    QStringList headers_{};
    QVector<StatsRow> rows_{};
};
