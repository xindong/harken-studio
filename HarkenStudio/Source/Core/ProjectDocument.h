#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

enum class ProjectDocumentContract {
    VersionedSchema,
    DevelopmentV9
};

struct ProjectDocumentData
{
    QString type;
    QString id;
    QString projectId;
    int schemaVersion = 0;
    QString studioVersion;
    QJsonObject header;
    QJsonObject content;
};

class ProjectDocument
{
public:
    static QJsonObject createHeader(const QString& type,
                                    const QString& id,
                                    const QString& projectId,
                                    const QJsonObject& originalHeader = {});
    static QJsonObject create(const QString& type,
                              const QString& id,
                              const QString& projectId,
                              const QJsonObject& content,
                              const QJsonObject& originalHeader = {});
    static bool parse(const QJsonObject& document,
                      const QString& expectedType,
                      ProjectDocumentData* data,
                      QString* error = nullptr,
                      ProjectDocumentContract contract = ProjectDocumentContract::VersionedSchema);
    static bool validateFieldGroupAuthoring(const QJsonObject& content,
                                              QString* error = nullptr);
    static void discardRetiredMakeUpGain(QJsonObject* properties);
    static bool normalizeContent(QJsonObject* content, QString* error = nullptr);
};

}
