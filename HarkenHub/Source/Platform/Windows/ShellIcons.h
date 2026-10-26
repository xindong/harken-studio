#pragma once

#include <QString>

namespace VNH {

inline constexpr wchar_t hubAppUserModelId[] = L"HarkenSound.HarkenHub";

void refreshShellIconsIfChanged();
void migrateShellShortcuts(const QString& taskbarIconPath = {});

}
