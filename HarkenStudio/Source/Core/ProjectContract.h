#pragma once

#include "ProjectIssue.h"
#include "ProjectFormatVersion.h"
#include "ProjectDocument.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace VNX {

struct ProjectContractProbe
{
    bool ok = false;
    ProjectIssue issue = ProjectIssue::None;
    QString detail;
    int schemaVersion = 0;
    QString projectId;
    QStringList blockingErrors;
    QStringList warnings;
};

class ProjectContractReadContext
{
public:
    void clear();

private:
    friend class ProjectContractRegistry;
    struct ParsedDocument
    {
        QByteArray bytes;
        QJsonObject object;
    };
    struct ValidatedDocument
    {
        int schemaVersion = 0;
        QString documentType;
        QJsonObject object;
        QStringList errors;
    };
    struct DecodedDocument
    {
        QString documentType;
        QJsonObject object;
        ProjectDocumentData data;
        QString error;
        bool ok = false;
    };
    QHash<QString, ParsedDocument> parsed_;
    QHash<QString, ValidatedDocument> validated_;
    QHash<QString, DecodedDocument> decoded_;
};

class ProjectContractRegistry
{
public:
    static ProjectContractRegistry& instance();

    ProjectContractProbe probe(const QString& projectFilePath,
                               ProjectContractReadContext* context = nullptr) const;
    QStringList validateTree(const QString& projectDirectory,
                             const QJsonObject& projectRoot,
                             bool requireCurrentSchema,
                             const QHash<QString, QJsonObject>& overrides = {},
                             ProjectContractReadContext* context = nullptr) const;
    QStringList validateSnapshot(const QString& projectDirectory,
                                 const QJsonObject& projectRoot,
                                 bool requireCurrentSchema,
                                 const QHash<QString, QJsonObject>& documents,
                                 ProjectContractReadContext* context = nullptr) const;

private:
    ProjectContractRegistry() = default;
    static bool readJsonObject(const QString& path, QJsonObject* object, QString* error,
                               ProjectContractReadContext* context);
    static QStringList validateDocument(int schemaVersion, const QString& documentType,
                                        const QJsonObject& document,
                                        const QString& relativePath,
                                        ProjectContractReadContext* context);
    static bool decodeDocument(const QJsonObject& document, const QString& documentType,
                                const QString& relativePath, ProjectDocumentData* data,
                                QString* error, ProjectContractReadContext* context);
    QStringList validateTreeImpl(const QString& projectDirectory,
                                 const QJsonObject& projectRoot,
                                 bool requireCurrentSchema,
                                 const QHash<QString, QJsonObject>& overrides,
                                 ProjectContractReadContext* context,
                                 bool includeVirtualDocuments) const;
};

}
