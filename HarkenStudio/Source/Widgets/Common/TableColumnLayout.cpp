#include "TableColumnLayout.h"

#include <algorithm>
#include <numeric>

namespace VNX {

QHash<QString, double>& columnShareStore()
{
    static QHash<QString, double> store;
    return store;
}

TableColumnLayout::TableColumnLayout(const QList<int>& weights)
    : defaults_(weights)
{
    for (int weight : weights) {
        Q_ASSERT(weight >= 0);
        weights_.append(weight);
    }
}

void TableColumnLayout::setKey(const QString& key, const QStringList& columns)
{
    Q_ASSERT(columns.size() == weights_.size());
    keys_.clear();
    for (qsizetype i = 0; i < columns.size(); ++i) {
        keys_.append(key.isEmpty() ? QString() : key + QLatin1Char('.') + columns[i]);
        weights_[i] = defaults_[i] == 0 ? 0
            : columnShareStore().value(keys_[i], defaults_[i]);
    }
}

QList<int> TableColumnLayout::widths(int available, const QList<int>& minimums,
                                    const QList<bool>& hidden, bool fitToWidth) const
{
    Q_ASSERT(minimums.size() == weights_.size());
    QList<int> result = minimums;
    for (int i = 0; i < result.size(); ++i)
        if (!hidden.isEmpty() && hidden[i]) result[i] = 0;
    const int totalMinimum = std::accumulate(result.begin(), result.end(), 0);
    if (fitToWidth && totalMinimum > available) {
        int compact = 0;
        for (int i = 0; i < result.size(); ++i)
            if (weights_[i] == 0) compact += result[i];
        const bool keepCompact = compact <= available;
        int remaining = std::max(0, available - (keepCompact ? compact : 0));
        int remainingMinimum = totalMinimum - (keepCompact ? compact : 0);
        for (int i = 0; i < result.size(); ++i) {
            if (keepCompact && weights_[i] == 0) continue;
            int& minimum = result[i];
            if (minimum == 0) continue;
            const int next = static_cast<int>(qint64(remaining) * minimum / remainingMinimum);
            remainingMinimum -= minimum;
            remaining -= next;
            minimum = next;
        }
        return result;
    }
    QList<int> pending;
    double totalWeight = 0;
    int remaining = std::max(available, std::accumulate(result.begin(), result.end(), 0));
    for (int i = 0; i < weights_.size(); ++i) {
        if (!hidden.isEmpty() && hidden[i]) continue;
        if (weights_[i] > 0) {
            pending.append(i);
            totalWeight += weights_[i];
        } else {
            remaining -= minimums[i];
        }
    }
    for (qsizetype i = 0; i < pending.size();) {
        const int column = pending[i];
        if (remaining * weights_[column] < minimums[column] * totalWeight) {
            remaining -= minimums[column];
            totalWeight -= weights_[column];
            pending.removeAt(i);
            i = 0;
        } else {
            ++i;
        }
    }
    int assigned = 0;
    double cumulativeWeight = 0;
    for (qsizetype i = 0; i < pending.size(); ++i) {
        const int column = pending[i];
        cumulativeWeight += weights_[column];
        const int boundary = i + 1 == pending.size() ? remaining
            : qRound(remaining * cumulativeWeight / totalWeight);
        result[column] = boundary - assigned;
        assigned = boundary;
    }
    return result;
}

void TableColumnLayout::resizeColumn(int column, int width, const QList<int>& current,
                                     const QList<int>& minimums, const QList<int>& visualOrder)
{
    Q_ASSERT(current.size() == weights_.size() && minimums.size() == weights_.size());
    if (column < 0 || column >= weights_.size() || weights_[column] <= 0) return;
    QList<int> order = visualOrder;
    if (order.isEmpty())
        for (int i = 0; i < weights_.size(); ++i) order.append(i);
    const int position = order.indexOf(column);
    QList<int> neighbors;
    for (int i = position + 1; i < order.size(); ++i)
        if (weights_[order[i]] > 0 && current[order[i]] > 0) neighbors.append(order[i]);
    if (neighbors.isEmpty()) {
        for (int i = position - 1; i >= 0; --i)
            if (weights_[order[i]] > 0 && current[order[i]] > 0) neighbors.append(order[i]);
    }
    if (neighbors.isEmpty()) return;
    QList<int> adjusted = current;
    int delta = std::max(width, minimums[column]) - current[column];
    if (delta < 0) {
        adjusted[neighbors.front()] -= delta;
        adjusted[column] += delta;
    } else {
        for (int neighbor : neighbors) {
            const int take = std::min(delta, std::max(0, adjusted[neighbor] - minimums[neighbor]));
            adjusted[neighbor] -= take;
            adjusted[column] += take;
            delta -= take;
        }
    }
    int span = 0;
    double totalWeight = 0;
    for (int i = 0; i < weights_.size(); ++i)
        if (weights_[i] > 0 && current[i] > 0) {
            span += adjusted[i];
            totalWeight += weights_[i];
        }
    if (span == 0) return;
    for (int i = 0; i < weights_.size(); ++i)
        if (weights_[i] > 0 && current[i] > 0)
            weights_[i] = totalWeight * adjusted[i] / span;
    save();
}

void TableColumnLayout::reset()
{
    for (int i = 0; i < weights_.size(); ++i) {
        weights_[i] = defaults_[i];
        if (!keys_.isEmpty()) columnShareStore().remove(keys_[i]);
    }
}

void TableColumnLayout::save()
{
    for (int i = 0; i < keys_.size(); ++i)
        if (!keys_[i].isEmpty() && weights_[i] > 0)
            columnShareStore().insert(keys_[i], weights_[i]);
}

}
