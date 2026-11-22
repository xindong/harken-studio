#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace harken::studio::cli {

QString commandDescriptionText(const QJsonObject& entry, bool shell = false);
QString commandDirectoryText(const QJsonArray& entries, bool shell = false);
QJsonObject filterCommandDirectory(QJsonObject response, const QString& search,
                                   const QString& category, bool readOnly);

}
