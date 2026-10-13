#include "BuiltinLayouts.h"

#include <QVector>

namespace VNX {
namespace BuiltinLayouts {
namespace {

const std::vector<Entry> kEntries {{
    {QStringLiteral("default_layout"),
     QStringLiteral("Default"),
     QStringLiteral("defaultLayoutAction")},
}};

bool matchesToken(const Entry& entry, QStringView token)
{
    return token.compare(entry.id, Qt::CaseInsensitive) == 0
        || token.compare(entry.reservedName, Qt::CaseInsensitive) == 0;
}

} // namespace

const std::vector<Entry>& entries()
{
    return kEntries;
}

const Entry* findByToken(QStringView token)
{
    if (token.isEmpty())
        return nullptr;
    for (const Entry& entry : kEntries) {
        if (matchesToken(entry, token))
            return &entry;
    }
    return nullptr;
}

bool isReservedToken(QStringView token)
{
    return findByToken(token) != nullptr;
}

QStringList ids()
{
    QStringList result;
    result.reserve(static_cast<int>(kEntries.size()));
    for (const Entry& entry : kEntries)
        result.append(entry.id);
    return result;
}

ResetTarget resolveResetTarget(QStringView requested, QStringView currentView)
{
    const QString effective = requested.isEmpty()
        ? currentView.toString()
        : requested.toString();
    if (const Entry* builtin = findByToken(effective))
        return {ResetTargetKind::Builtin, builtin->id};
    if (!effective.isEmpty() && effective == currentView)
        return {ResetTargetKind::UserCurrent, effective};
    return {ResetTargetKind::Invalid, effective};
}

QVector<LayoutStateItem> describeLayoutState(QStringView currentView)
{
    QVector<LayoutStateItem> items;
    const QString current = currentView.toString();
    const QStringList layoutIds = ids();
    items.reserve(layoutIds.size() + 1);
    for (int i = 0; i < layoutIds.size(); ++i)
        items.push_back({layoutIds[i], i, layoutIds[i] == current});
    if (!findByToken(current) && !current.isEmpty())
        items.push_back({current, static_cast<int>(layoutIds.size()), true});
    return items;
}

} // namespace BuiltinLayouts
} // namespace VNX
