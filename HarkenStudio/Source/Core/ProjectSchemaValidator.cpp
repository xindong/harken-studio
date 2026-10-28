#include "ProjectSchemaValidator.h"
#include "ProjectDocument.h"
#include "ProjectFormatVersion.h"

#include "ProjectSchemaValidator.generated.h"

namespace VNX {

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
    QJsonValue input = document;
    if (version == kCurrentProjectSchemaVersion && document.isObject()) {
        auto object = document.toObject();
        if (object.value(QStringLiteral("content")).isObject()) {
            auto content = object.value(QStringLiteral("content")).toObject();
            QString error;
            if (!ProjectDocument::normalizeContent(&content, &error))
                return {relativePath + QStringLiteral(": ") + error};
            object.insert(QStringLiteral("content"), content);
            input = object;
        }
    }
    GeneratedSchemaRegistry::validateProjectDocument(
        version, documentType, input, relativePath, errors);
    return errors;
}

QStringList ProjectSchemaValidator::validatePackManifest(
    int version, const QJsonValue& document, const QString& relativePath)
{
    QStringList errors;
    GeneratedSchemaRegistry::validatePackManifest(
        version, document, relativePath, errors);
    return errors;
}

}
