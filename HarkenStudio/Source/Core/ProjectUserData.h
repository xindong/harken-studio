#pragma once

#include <QJsonObject>
#include <QString>

namespace harken::studio::ProjectUserData {

inline constexpr char kDirectory[] = ".user";
inline constexpr char kEditorLayout[] = "editor_layout.json";
inline constexpr char kPackExportSelection[] = "pack-export-selection.json";

QJsonObject read(const QString& projectRoot, const QString& fileName);
bool write(const QString& projectRoot, const QString& fileName, QJsonObject data);
bool importFromCache(const QString& projectRoot);

}
