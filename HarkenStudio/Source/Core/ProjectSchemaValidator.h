#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace VNX {

class ProjectSchemaValidator
{
public:
    static bool supportsProjectVersion(int version);
    static bool supportsPackVersion(int version);

    static QStringList validateProjectDocument(int version,
                                               const QString& documentType,
                                               const QJsonValue& document,
                                               const QString& relativePath);
    static QStringList validatePackManifest(int version,
                                            const QJsonValue& document,
                                            const QString& relativePath);
};

}
