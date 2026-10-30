#pragma once

#include "ProjectContract.h"
#include <QFileDevice>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace VNX {

struct ProjectSaveBaseline
{
    QString projectPath;
    QHash<QString, QByteArray> files;
    QHash<QString, QByteArray> hashes;
    QSet<QString> directories;
    QSet<QUuid> normalizedDocuments;
    ProjectContractValidationBaseline validation;
    ProjectContractReadContext context;
};

struct ProjectSavePlan
{
    QJsonObject projectDocument;
    QHash<QString, QJsonObject> documents;
    QHash<QString, QByteArray> files;
    QHash<QString, QFileDevice::Permissions> permissions;
    QSet<QString> removed;
    QSet<QString> directories;
    QHash<QUuid, QJsonObject> serializedItems;
    QSet<QUuid> acceptedItems;
    ProjectContractValidationBaseline validation;
    ProjectContractValidationDetails validationDetails;

    static QByteArray encode(const QJsonObject& document)
    {
        QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
#ifdef Q_OS_WIN
        bytes.replace("\n", "\r\n");
#endif
        return bytes;
    }

    void addDocument(const QString& path, const QJsonObject& document,
                     const ProjectSaveBaseline* baseline, bool overwrite = false)
    {
        if (!overwrite && baseline
            && baseline->validation.documents().value(path) == document)
            return;
        documents.insert(path, document);
        files.insert(path, encode(document));
    }

    void addFile(const QString& path, const QByteArray& bytes,
                 const ProjectSaveBaseline* baseline, bool overwrite = false)
    {
        if (overwrite || !baseline || baseline->files.value(path) != bytes
            || !baseline->files.contains(path))
            files.insert(path, bytes);
    }
};

}
