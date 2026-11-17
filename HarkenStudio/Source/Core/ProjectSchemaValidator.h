#pragma once

#include "ProjectDocument.h"

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
                                               const QString& relativePath,
                                               ProjectDocumentContract contract = ProjectDocumentContract::VersionedSchema);
    static QStringList validatePackMetadata(int version,
                                            const QJsonValue& document,
                                            const QString& relativePath);
};

}
