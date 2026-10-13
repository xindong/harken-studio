#pragma once

#include <QString>

namespace VNX {
namespace EditorLaunchMap {

QString editorPanelTypeForSlot(const QString& slot);
void setEditorTypeDisabled(const QString& panelType, bool disabled,
                           const QString& reason = QString());
bool isSlotDisabled(const QString& slot);
QString slotDisabledReason(const QString& slot);

}
}
