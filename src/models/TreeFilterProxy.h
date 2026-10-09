#pragma once

#include <QSortFilterProxyModel>

#include "../models/FileSystemModel.h"

// Tree filtering with two independent criteria: quick-search text and an
// extension filter (".log"). A row passes when its name contains the
// search text (case-insensitive) AND — for files only — ends with the
// extension when one is set. Directories ignore the extension (they are
// containers); with recursive filtering enabled, ancestor chains of
// matches stay visible while everything else hides.
class TreeFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit TreeFilterProxyModel(QObject* parent = nullptr);

    void setSourceModel(QAbstractItemModel* source) override;

    void setSearchText(const QString& text);
    void setExtensionFilter(const QString& ext); // e.g. ".log"; empty clears
    QString extensionFilter() const { return ext_; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString search_{};
    QString ext_{};
    FileSystemModel* files_{nullptr};
};
