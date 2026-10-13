#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

class SchemaArchive
{
public:
    static SchemaArchive& instance();

    QString rootPath() const;
    bool verifyBuildSnapshot(QString* error = nullptr) const;
    QJsonObject buildManifest(QString* error = nullptr) const;
    QJsonObject projectSchema(int version, QString* error = nullptr) const;
    QJsonObject packSchema(int version, QString* error = nullptr) const;
};

}
