#pragma once

#include <QSortFilterProxyModel>

#include <QString>

// Tree filtering by quick-search text. A row passes when its name
// contains the text (case-insensitive); with recursive filtering
// enabled, ancestor chains of matches stay visible while everything
// else hides.
class TreeFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit TreeFilterProxyModel(QObject* parent = nullptr);

    void setSearchText(const QString& text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString search_{};
};
