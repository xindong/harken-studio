#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

namespace harken::studio::ProjectUserData {

inline constexpr char kDirectory[] = ".user";
inline constexpr char kEditorLayout[] = "editor_layout.json";
inline constexpr char kPackExportSelection[] = "pack-export-selection.json";

void load(const QString& projectRoot, const QString& fileName,
          const std::function<bool(const QJsonObject&)>& restore, const QString& legacySettingsKey = {});
bool write(const QString& projectRoot, const QString& fileName, QJsonObject data);
bool prepareCacheReset(const QString& projectRoot);

}
