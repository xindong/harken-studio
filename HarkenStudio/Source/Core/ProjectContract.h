#pragma once

#include "ProjectIssue.h"
#include "ProjectFormatVersion.h"
#include "ProjectDocument.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
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

class ProjectContractValidationBaseline
{
public:
    bool isValid() const { return valid_; }
    void clear();
    const QJsonObject& projectRoot() const { return rootDocument_; }
    const QHash<QString, QJsonObject>& documents() const { return documents_; }
    QStringList dependencyPaths(const QStringList& changedPaths) const;

private:
    friend class ProjectContractRegistry;
    bool valid_ = false;
    bool requireCurrentSchema_ = false;
    int schemaVersion_ = 0;
    QString rootRelativePath_;
    QJsonObject rootDocument_;
    QHash<QString, QJsonObject> documents_;
    QHash<QString, QJsonArray> semanticProjections_;
    QHash<QString, QString> pathsByGuid_;
    QHash<QString, QSet<QString>> dependencies_;
    QHash<QString, QSet<QString>> inheritedDependencies_;
    QHash<QString, QString> parentPaths_;
};

struct ProjectContractValidationDetails
{
    bool fullValidation = false;
    QStringList readPaths;
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
                             ProjectContractReadContext* context = nullptr,
                             ProjectContractValidationBaseline* baseline = nullptr,
                             const QString& rootRelativePath = {}) const;
    QStringList validateSnapshot(const QString& projectDirectory,
                                 const QJsonObject& projectRoot,
                                 bool requireCurrentSchema,
                                 const QHash<QString, QJsonObject>& documents,
                                 ProjectContractReadContext* context = nullptr,
                                 ProjectContractValidationBaseline* baseline = nullptr,
                                 const QString& rootRelativePath = {},
                                 bool includePhysicalDocuments = true) const;
    QStringList validateIncrementalSnapshot(
        const QString& projectDirectory, const QJsonObject& projectRoot,
        bool requireCurrentSchema, const QHash<QString, QJsonObject>& changedDocuments,
        const QSet<QString>& removedPaths,
        const ProjectContractValidationBaseline& baseline,
        ProjectContractValidationBaseline* validatedBaseline,
        ProjectContractReadContext* context = nullptr,
        ProjectContractValidationDetails* details = nullptr) const;

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
                                 bool includeVirtualDocuments,
                                 ProjectContractValidationBaseline* baseline,
                                 bool includePhysicalDocuments = true,
                                 bool reuseGlobalValidation = false,
                                 const QString& knownRootRelativePath = {}) const;
};

}
