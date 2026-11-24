#include "ProjectSchemaValidator.h"

#include "ProjectSchemaValidator.generated.h"

namespace harken::studio {

bool ProjectSchemaValidator::supportsProjectVersion(int version)
{
    return GeneratedSchemaRegistry::supportsProjectVersion(version);
}

bool ProjectSchemaValidator::supportsPackVersion(int version)
{
    return GeneratedSchemaRegistry::supportsPackVersion(version);
}

QStringList ProjectSchemaValidator::validateProjectDocument(
    int version, const QString& documentType, const QJsonValue& document,
    const QString& relativePath)
{
    QStringList errors;
    GeneratedSchemaRegistry::validateProjectDocument(
        version, documentType, document, relativePath, errors);
    return errors;
}

QStringList ProjectSchemaValidator::validatePackMetadata(
    int version, const QJsonValue& document, const QString& relativePath)
{
    QStringList errors;
    GeneratedSchemaRegistry::validatePackMetadata(
        version, document, relativePath, errors);
    return errors;
}

}
