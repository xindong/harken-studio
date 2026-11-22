#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

namespace harken::studio {

bool prepareConversionMigration(const QString& sourceDirectory,
                                const QString& projectFile,
                                QMap<QString, QJsonObject>& documents,
                                QHash<QString, QByteArray>& inputFingerprints,
                                QStringList* warnings,
                                QString* error);

}
