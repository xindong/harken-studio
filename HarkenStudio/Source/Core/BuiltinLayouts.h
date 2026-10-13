#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <vector>

namespace VNX {
namespace BuiltinLayouts {

struct Entry
{
    QString id;
    QString reservedName;
    QString menuActionObjectName;
};

const std::vector<Entry>& entries();
const Entry* findByToken(QStringView token);
bool isReservedToken(QStringView token);
QStringList ids();

enum class ResetTargetKind
{
    Builtin,
    UserCurrent,
    Invalid,
};

struct ResetTarget
{
    ResetTargetKind kind = ResetTargetKind::Invalid;
    QString canonical;
};

// Empty requested uses currentView. Builtin tokens normalize to the table id.
// User layout names stay case-sensitive and must equal currentView.
ResetTarget resolveResetTarget(QStringView requested, QStringView currentView);

struct LayoutStateItem
{
    QString name;
    int index = 0;
    bool isCurrent = false;
};

// Built-in ids first; a non-built-in currentView is appended as the current user layout.
QVector<LayoutStateItem> describeLayoutState(QStringView currentView);

} // namespace BuiltinLayouts
} // namespace VNX
