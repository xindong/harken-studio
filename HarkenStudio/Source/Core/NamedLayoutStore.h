
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace VNX {

struct NamedLayoutInfo
{
    QString name;
    QString savedAt;
    QString filePath;
};

namespace NamedLayoutStore {

QString dirPath();
void setDirPathOverrideForTesting(const QString& path);
QList<NamedLayoutInfo> list();
bool isReservedName(const QString& name);
bool read(const QString& name, QJsonObject* jsonOut);
bool write(const QString& name, const QJsonObject& json, QString* pathOut = nullptr);
bool remove(const QString& name);
bool rename(const QString& oldName, const QString& newName);

}

}
