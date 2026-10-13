#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace VNX {

QHash<QString, double>& columnShareStore();

class TableColumnLayout
{
public:
    explicit TableColumnLayout(const QList<int>& weights = {});
    void setKey(const QString& key, const QStringList& columns);
    QList<int> widths(int available, const QList<int>& minimums,
                      const QList<bool>& hidden = {}, bool fitToWidth = false) const;
    void resizeColumn(int column, int width, const QList<int>& current,
                      const QList<int>& minimums, const QList<int>& visualOrder = {});
    void reset();

private:
    void save();

    QList<int> defaults_;
    QList<double> weights_;
    QStringList keys_;
};

}
