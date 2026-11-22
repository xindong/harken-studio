#pragma once

#include <QString>
#include <QIcon>

namespace harken::hub {

inline constexpr wchar_t hubAppUserModelId[] = L"HarkenSound.HarkenHub";

QString saveShellIcon(const QIcon& icon, const QString& directory);
enum class ShortcutMigrationResult { Complete, Busy, Failed };
void refreshShellIconsIfChanged();
ShortcutMigrationResult migrateShellShortcuts(const QString& taskbarIconPath = {});

}
