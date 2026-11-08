#include "EditorLaunchMap.h"

#include <QHash>

namespace VNX {
namespace EditorLaunchMap {

namespace {
QHash<QString, QString>& disabledTypes()
{
    static QHash<QString, QString> types;
    return types;
}
}

QString editorPanelTypeForSlot(const QString& slot)
{
    static const QHash<QString, QString> map = {
        { QStringLiteral("attenuation"), QStringLiteral("ExternalEditor") },
        { QStringLiteral("timeline"), QStringLiteral("TimelineEditor") },
        { QStringLiteral("selections"), QStringLiteral("GridEditor") },
    };
    auto it = map.find(slot);
    return it != map.end() ? it.value() : QString();
}

void setEditorTypeDisabled(const QString& panelType, bool disabled, const QString& reason)
{
    if (disabled)
        disabledTypes().insert(panelType, reason);
    else
        disabledTypes().remove(panelType);
}

bool isSlotDisabled(const QString& slot)
{
    return disabledTypes().contains(editorPanelTypeForSlot(slot));
}

QString slotDisabledReason(const QString& slot)
{
    return disabledTypes().value(editorPanelTypeForSlot(slot));
}

}
}
