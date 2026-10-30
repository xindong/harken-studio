#include "ProjectContract.h"
#include "BucketFolderStore.h"
#include "ProjectPlatform.h"
#include "ComponentCatalog.h"
#include "ProjectDocument.h"
#include "ProjectDataPolicy.h"
#include "ProjectSchemaRules.h"
#include "ProjectSchemaValidator.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace VNX {

namespace {

class ContractRuleExecutor final : public ProjectSchemaRuleExecutor
{
public:
    std::function<void(const QStringList&)> uniqueAcrossProjectCheck;
    std::function<void(const QStringList&)> documentIdMirrorsRootCheck;
    std::function<void()> projectIdUniformCheck;
    std::function<void(const QList<ProjectSchemaReferenceRule>&)> referencesResolveCheck;
    std::function<void(const QStringList&)> acyclicCheck;
    std::function<void(const QString&, const QStringList&, const QString&,
                       const QString&)> singletonCheck;
    std::function<void()> schemaVersionUniformCheck;

    void uniqueAcrossProject(const QStringList& fields) override
    {
        uniqueAcrossProjectCheck(fields);
    }
    void documentIdMirrorsRoot(const QStringList& documentTypes) override
    {
        documentIdMirrorsRootCheck(documentTypes);
    }
    void projectIdUniform() override
    {
        projectIdUniformCheck();
    }
    void referencesResolve(const QList<ProjectSchemaReferenceRule>& references) override
    {
        referencesResolveCheck(references);
    }
    void acyclic(const QStringList& referencePath) override
    {
        acyclicCheck(referencePath);
    }
    void singleton(const QString& documentType,
                   const QStringList& predicatePath,
                   const QString& expectedValue,
                   const QString& message) override
    {
        singletonCheck(documentType, predicatePath, expectedValue, message);
    }
    void schemaVersionUniform() override
    {
        schemaVersionUniformCheck();
    }
};

QStringList managedPackDirectories()
{
    return {QStringLiteral("Actions"), QStringLiteral("Attenuations"),
            QStringLiteral("Busses"), QStringLiteral("Layers"),
            QStringLiteral("Variables")};
}

QString domainForFolder(const QString& folder)
{
    if (folder.compare(QStringLiteral("Layers"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.layer");
    if (folder.compare(QStringLiteral("Busses"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.bus");
    if (folder.compare(QStringLiteral("Actions"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.action");
    if (folder.compare(QStringLiteral("Variables"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.variable");
    if (folder.compare(QStringLiteral("Attenuations"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.attenuation");
    return {};
}

QString documentTypeForFolder(const QString& folder)
{
    if (folder.compare(QStringLiteral("Layers"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("project.layer");
    if (folder.compare(QStringLiteral("Busses"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("project.bus");
    if (folder.compare(QStringLiteral("Actions"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("project.action");
    if (folder.compare(QStringLiteral("Variables"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("project.variable");
    if (folder.compare(QStringLiteral("Attenuations"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("project.attenuation");
    return {};
}

QString packDomain(const QJsonObject& object, int schemaVersion)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type.compare(QStringLiteral("Sound"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Container"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Track"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Segment"), Qt::CaseInsensitive) == 0
        || (schemaVersion < kCurrentProjectSchemaVersion
            && type.compare(QStringLiteral("Piece"), Qt::CaseInsensitive) == 0)
        || type.compare(QStringLiteral("Layer"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.layer");
    if (type.compare(QStringLiteral("Bus"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.bus");
    if (type.compare(QStringLiteral("Action"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.action");
    if (type.compare(QStringLiteral("Variable"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.variable");
    if (type.compare(QStringLiteral("Attenuation"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pack.attenuation");
    return {};
}

}

void ProjectContractReadContext::clear()
{
    parsed_.clear();
    validated_.clear();
    decoded_.clear();
}

bool ProjectContractRegistry::readJsonObject(
    const QString& path, QJsonObject* object, QString* error,
    ProjectContractReadContext* context)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (context) {
        const auto previous = context->parsed_.constFind(path);
        if (previous != context->parsed_.cend() && previous->bytes == bytes) {
            if (object)
                *object = previous->object;
            return true;
        }
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid JSON in %1: %2")
                         .arg(path, parseError.errorString());
        return false;
    }
    if (object)
        *object = document.object();
    if (context)
        context->parsed_.insert(path, {bytes, document.object()});
    return true;
}

QStringList ProjectContractRegistry::validateDocument(
    int schemaVersion, const QString& documentType,
    const QJsonObject& document, const QString& relativePath,
    ProjectContractReadContext* context)
{
    if (context) {
        const auto previous = context->validated_.constFind(relativePath);
        if (previous != context->validated_.cend()
            && previous->schemaVersion == schemaVersion
            && previous->documentType == documentType
            && previous->object == document)
            return previous->errors;
    }
    QStringList errors = ProjectSchemaValidator::validateProjectDocument(
        schemaVersion, documentType, document, relativePath);
    if (context)
        context->validated_.insert(relativePath, {schemaVersion, documentType, document, errors});
    return errors;
}

bool ProjectContractRegistry::decodeDocument(
    const QJsonObject& document, const QString& documentType,
    const QString& relativePath, ProjectDocumentData* data, QString* error,
    ProjectContractReadContext* context)
{
    if (context) {
        const auto previous = context->decoded_.constFind(relativePath);
        if (previous != context->decoded_.cend()
            && previous->documentType == documentType && previous->object == document) {
            if (data) *data = previous->data;
            if (error) *error = previous->error;
            return previous->ok;
        }
    }
    ProjectDocumentData parsed;
    QString detail;
    const bool ok = ProjectDocument::parse(document, documentType, &parsed, &detail);
    if (data) *data = parsed;
    if (error) *error = detail;
    if (context)
        context->decoded_.insert(relativePath, {documentType, document, parsed, detail, ok});
    return ok;
}

ProjectContractRegistry& ProjectContractRegistry::instance()
{
    static ProjectContractRegistry registry;
    return registry;
}

ProjectContractProbe ProjectContractRegistry::probe(
    const QString& projectFilePath, ProjectContractReadContext* context) const
{
    ProjectContractProbe result;
    QJsonObject rootDocument;
    QString readError;
    if (!readJsonObject(projectFilePath, &rootDocument, &readError, context)) {
        result.blockingErrors.append(readError);
        return result;
    }

    if (!rootDocument.contains(QStringLiteral("harkenDocument"))) {
        result.issue = ProjectIssue::ProjectHasNoFormalBaseline;
        result.detail = QStringLiteral("harkenDocument is missing");
        result.blockingErrors.append(projectIssueCode(result.issue) + QChar(':') + result.detail);
        return result;
    }

    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(rootDocument, QStringLiteral("project.root"),
                                &document, &documentError)) {
        result.issue = ProjectIssue::ProjectFileDamaged;
        result.detail = documentError;
        result.blockingErrors.append(projectIssueCode(result.issue) + QChar(':') + result.detail);
        return result;
    }
    result.schemaVersion = document.schemaVersion;
    result.projectId = document.id;
    if (document.projectId != document.id) {
        result.issue = ProjectIssue::ProjectSchemaStateMismatch;
        result.detail = QStringLiteral("Project root identity does not match its project identity");
        result.blockingErrors.append(result.detail);
        return result;
    }
    if (result.schemaVersion <= ProjectSchema::kProjectSchemaVersion
        && !ProjectSchemaValidator::supportsProjectVersion(result.schemaVersion)) {
        result.issue = ProjectIssue::ProjectSchemaUnsupported;
        result.detail = QStringLiteral("Project Schema %1 is no longer supported")
                            .arg(result.schemaVersion);
        result.blockingErrors.append(projectIssueCode(result.issue) + QChar(':')
                                     + result.detail);
        return result;
    }
    if (result.schemaVersion == ProjectSchema::kProjectSchemaVersion) {
        const auto platformErrors = ProjectPlatformSemantics::validateDocuments(document.content, {});
        if (!platformErrors.isEmpty()) {
            result.issue = ProjectIssue::ProjectFileDamaged;
            result.detail = platformErrors.first();
            result.blockingErrors = platformErrors;
            return result;
        }
    }
    const QDir directory(QFileInfo(projectFilePath).absolutePath());
    for (const QString& folder : managedPackDirectories()) {
        QDirIterator files(directory.filePath(folder), {QStringLiteral("*.vp")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QJsonObject pack;
            ProjectDocumentData parsed;
            QString detail;
            if (!readJsonObject(path, &pack, &detail, context)
                || !ProjectDocument::parse(pack, documentTypeForFolder(folder), &parsed, &detail)) {
                result.issue = ProjectIssue::ProjectFileDamaged;
                result.detail = QStringLiteral("%1: %2").arg(directory.relativeFilePath(path), detail);
                result.blockingErrors.append(result.detail);
                return result;
            }
        }
    }
    result.ok = true;
    return result;
}

QStringList ProjectContractRegistry::validateTree(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema,
    const QHash<QString, QJsonObject>& overrides,
    ProjectContractReadContext* context) const
{
    return validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
                            overrides, context, false);
}

QStringList ProjectContractRegistry::validateSnapshot(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema, const QHash<QString, QJsonObject>& documents,
    ProjectContractReadContext* context) const
{
    return validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
                            documents, context, true);
}

QStringList ProjectContractRegistry::validateTreeImpl(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema, const QHash<QString, QJsonObject>& overrides,
    ProjectContractReadContext* context, bool includeVirtualDocuments) const
{
    QStringList errors;
    const QDir directory(projectDirectory);
    const QStringList projectFiles = directory.entryList(
        {QStringLiteral("*.hsp")}, QDir::Files, QDir::Name);
    const QString rootRelativePath = projectFiles.isEmpty()
        ? directory.dirName() + QStringLiteral(".hsp")
        : projectFiles.first();
    ProjectDocumentData rootDocument;
    QString rootDocumentError;
    if (!decodeDocument(projectRootDocument, QStringLiteral("project.root"),
                         rootRelativePath, &rootDocument, &rootDocumentError, context)) {
        errors.append(rootDocumentError);
        if (includeVirtualDocuments && context) context->clear();
        return errors;
    }
    QSet<QString> validatedPaths{rootRelativePath};
    const int schemaVersion = rootDocument.schemaVersion;
    const QJsonObject projectRoot = rootDocument.content;
    errors.append(validateDocument(
        schemaVersion, QStringLiteral("project.root"), projectRootDocument,
        rootRelativePath, context));

    struct NodeRecord {
        QString domain;
        QString path;
        QJsonObject object;
        bool layerRoot = false;
    };
    struct PackRecord {
        QString domain;
        QString documentType;
        QString path;
        QJsonObject object;
        QString documentId;
        QString projectId;
        int schemaVersion = 0;
    };
    QSet<QString> guids;
    QHash<QString, QString> documentByGuid;
    QHash<QString, QString> typeByGuid;
    QMap<QString, QStringList> guidPaths;
    QMap<QString, QStringList> documentIdPaths{
        {rootDocument.id, {rootRelativePath}}};
    QList<NodeRecord> nodes;
    QList<PackRecord> packs;

    const auto validateComponents = [&](const QJsonObject& object,
                                        const QString& path) {
        if (!object.value(QStringLiteral("components")).isObject()) {
            errors.append(QStringLiteral("Components are malformed in %1").arg(path));
            return;
        }
        const QJsonObject components = object.value(QStringLiteral("components"))
                                            .toObject();
        for (auto it = components.constBegin(); it != components.constEnd(); ++it) {
            if (!ComponentCatalog::parseComponentName(it.key()).has_value()
                && !(schemaVersion < kCurrentProjectSchemaVersion
                     && it.key() == QLatin1String("piecetimeline")))
                errors.append(QStringLiteral("Unknown component %1 in %2")
                                  .arg(it.key(), path));
            else if (!it.value().isObject())
                errors.append(QStringLiteral("Component %1 is malformed in %2")
                                  .arg(it.key(), path));
        }
    };

    auto registerIdentity = [&](const QJsonObject& object,
                                const QString& path,
                                const QString& label) {
        const QJsonValue guidValue = object.value(QStringLiteral("guid"));
        const QUuid guid = QUuid::fromString(guidValue.toString());
        if (!guidValue.isString() || guid.isNull()) {
            errors.append(QStringLiteral("%1 guid is missing or invalid in %2")
                              .arg(label, path));
        } else {
            const QString key = guid.toString(QUuid::WithoutBraces);
            guids.insert(key);
            guidPaths[key].append(path);
        }

    };

    std::function<void(const QJsonObject&, const QString&, bool)> collectLayerNode;
    collectLayerNode = [&](const QJsonObject& object,
                           const QString& path,
                           bool layerRoot) {
        if (object.isEmpty()) {
            errors.append(QStringLiteral("Layer payload is missing in %1").arg(path));
            return;
        }
        if (object.contains(QStringLiteral("components")))
            validateComponents(object, path);
        registerIdentity(object, path, QStringLiteral("Layer item"));
        const QString nodeDomain = packDomain(object, schemaVersion);
        if (nodeDomain != QStringLiteral("pack.layer"))
            errors.append(QStringLiteral("Layer item type is invalid in %1").arg(path));
        nodes.append({QStringLiteral("pack.layer"), path, object, layerRoot});

        const QJsonValue children = object.value(QStringLiteral("children"));
        if (!children.isArray()) {
            errors.append(QStringLiteral("Layer children are missing or invalid in %1")
                              .arg(path));
        } else if (!children.toArray().isEmpty()) {
            errors.append(QStringLiteral("Inline Layer children are not supported in %1")
                              .arg(path));
            for (qsizetype index = 0; index < children.toArray().size(); ++index)
                collectLayerNode(children.toArray().at(index).toObject(),
                                 QStringLiteral("%1.children[%2]").arg(path).arg(index), false);
        }
    };

    if (QFileInfo(projectDirectory).isSymLink())
        errors.append(QStringLiteral("Project root cannot be a symbolic link"));
    QHash<QString, QStringList> virtualPaths;
    QHash<QString, QString> virtualPathKeys;
    if (includeVirtualDocuments) {
        for (auto entry = overrides.cbegin(); entry != overrides.cend(); ++entry) {
            const QString path = entry.key();
            if (!ProjectDataPolicy::isSafeRelativePath(path)
                || ProjectDataPolicy::normalizeRelativePath(path) != path) {
                errors.append(QStringLiteral("Invalid project document path: %1").arg(path));
                continue;
            }
            if (QFileInfo(path).suffix().compare(QStringLiteral("vp"), Qt::CaseInsensitive) != 0)
                continue;
            const QString folder = path.section(QLatin1Char('/'), 0, 0);
            if (domainForFolder(folder).isEmpty()) {
                errors.append(QStringLiteral("Pack file is outside a managed domain: %1").arg(path));
                continue;
            }
            for (const QString& managed : managedPackDirectories())
                if (folder.compare(managed, Qt::CaseInsensitive) == 0)
                    virtualPaths[managed].append(path);
            const QString key = BucketFolderStore::pathComparisonKey(path);
            if (virtualPathKeys.contains(key))
                errors.append(QStringLiteral("Duplicate project document path: %1").arg(path));
            virtualPathKeys.insert(key, path);
        }
    }
    for (const QString& folder : managedPackDirectories()) {
        const QDir packDirectory(directory.filePath(folder));
        if (!packDirectory.exists() && virtualPaths.value(folder).isEmpty())
            continue;
        QStringList paths;
        QDirIterator files(packDirectory.absolutePath(),
                           {QStringLiteral("*.vp")},
                           QDir::Files | QDir::Readable,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo fileInfo(files.next());
            paths.append(QDir::fromNativeSeparators(
                directory.relativeFilePath(fileInfo.absoluteFilePath())));
        }
        if (includeVirtualDocuments) {
            QSet<QString> physicalPaths;
            for (QString& path : paths) {
                const QString key = BucketFolderStore::pathComparisonKey(path);
                physicalPaths.insert(key);
                const auto replacement = virtualPathKeys.constFind(key);
                if (replacement != virtualPathKeys.cend()) path = replacement.value();
            }
            QStringList additional;
            for (const QString& path : virtualPaths.value(folder))
                if (!physicalPaths.contains(BucketFolderStore::pathComparisonKey(path)))
                    additional.append(path);
            additional.sort();
            paths.append(additional);
        }
        for (const QString& path : paths) {
            const QFileInfo fileInfo(directory.filePath(path));
            QJsonObject packDocument;
            QString readError;
            if (overrides.contains(path)) {
                packDocument = overrides.value(path);
            } else if (!readJsonObject(fileInfo.absoluteFilePath(), &packDocument,
                                       &readError, context)) {
                errors.append(readError);
                continue;
            }
            const QString documentType = documentTypeForFolder(folder);
            validatedPaths.insert(path);
            errors.append(validateDocument(
                schemaVersion, documentType, packDocument, path, context));
            ProjectDocumentData document;
            QString documentError;
            if (!decodeDocument(packDocument, documentType, path,
                                 &document, &documentError, context)) {
                errors.append(QStringLiteral("%1: %2").arg(path, documentError));
                continue;
            }
            documentIdPaths[document.id].append(path);
            const QJsonObject pack = document.content;
            const QString contentGuid = QUuid::fromString(
                pack.value(QStringLiteral("guid")).toString())
                                            .toString(QUuid::WithoutBraces);
            const QString domain = packDomain(pack, schemaVersion);
            if (domain.isEmpty()) {
                errors.append(QStringLiteral("Pack type is unknown in %1")
                                  .arg(fileInfo.fileName()));
            } else if (domain != domainForFolder(folder)) {
                errors.append(QStringLiteral("Pack type does not match its folder in %1")
                                  .arg(fileInfo.fileName()));
            }
            if (!pack.value(QStringLiteral("name")).isString()
                || pack.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                errors.append(QStringLiteral("Pack name is missing in %1").arg(fileInfo.fileName()));
            packs.append({domain, documentType, path, pack, document.id, document.projectId,
                          document.schemaVersion});
            if (domain == QStringLiteral("pack.layer")) {
                const QJsonObject data = pack;
                if (data.isEmpty()) {
                    errors.append(QStringLiteral("Layer content is missing in %1").arg(path));
                } else {
                    collectLayerNode(data, path + QStringLiteral(".content"), true);
                }
            } else {
                if (pack.contains(QStringLiteral("components")))
                    validateComponents(pack, path);
                registerIdentity(pack, path, QStringLiteral("Pack"));
            }
            if (!contentGuid.isEmpty()) {
                documentByGuid.insert(contentGuid, document.id);
                typeByGuid.insert(contentGuid,
                                  pack.value(QStringLiteral("type")).toString());
            }
        }
    }

    const auto rejectCycles = [&errors](const QHash<QString, QString>& parents,
                                        const QString& label) {
        QHash<QString, int> marks;
        std::function<bool(const QString&)> visit = [&](const QString& id) {
            if (id.isEmpty()) return true;
            const int mark = marks.value(id, 0);
            if (mark == 1) {
                errors.append(QStringLiteral("%1 parent references contain a cycle")
                                  .arg(label));
                return false;
            }
            if (mark == 2) return true;
            marks.insert(id, 1);
            const QString parent = parents.value(id);
            if (!parent.isEmpty() && !visit(parent)) return false;
            marks.insert(id, 2);
            return true;
        };
        for (auto it = parents.cbegin(); it != parents.cend(); ++it)
            if (!visit(it.key())) return;
    };
    const auto validateReference = [&](const QJsonValue& value,
                                       const QString& path,
                                       const QStringList& allowedTypes,
                                       bool allowNull = false) {
        if (allowNull && value.isNull()) return;
        if (!value.isObject()) {
            errors.append(QStringLiteral("Reference is malformed in %1").arg(path));
            return;
        }
        const QJsonObject reference = value.toObject();
        const QString id = QUuid::fromString(
            reference.value(QStringLiteral("id")).toString())
                               .toString(QUuid::WithoutBraces);
        if (id.isEmpty() || !guids.contains(id)) {
            errors.append(QStringLiteral("Reference target is missing in %1").arg(path));
            return;
        }
        if (!allowedTypes.isEmpty() && !allowedTypes.contains(typeByGuid.value(id))) {
            errors.append(QStringLiteral("Reference target has the wrong type in %1")
                              .arg(path));
        }
        if (reference.contains(QStringLiteral("documentId"))) {
            const QString documentId = QUuid::fromString(
                reference.value(QStringLiteral("documentId")).toString())
                                           .toString(QUuid::WithoutBraces);
            if (documentId.isEmpty() || documentId != documentByGuid.value(id))
                errors.append(QStringLiteral("Reference document does not match its target in %1")
                                  .arg(path));
        }
    };

    const auto validateGuidReference = [&](const QJsonValue& value,
                                           const QString& path,
                                           const QStringList& allowedTypes,
                                           const QJsonObject& owner,
                                           const QString& playbackKind) {
        const QString id = QUuid::fromString(value.toString())
                               .toString(QUuid::WithoutBraces);
        const auto report = [&](const QString& reason) {
            if (playbackKind.isEmpty()) {
                errors.append(reason + QStringLiteral(" in %1").arg(path));
            } else {
                errors.append(projectIssueCode(ProjectIssue::InvalidPlaybackReference)
                    + QStringLiteral(": Owner '%1' (%2), %3 at %4, target %5: %6")
                          .arg(owner.value(QStringLiteral("name")).toString(),
                               owner.value(QStringLiteral("guid")).toString(),
                               playbackKind, path, value.toString(), reason));
            }
        };
        if (!value.isString() || id.isEmpty() || !guids.contains(id)) {
            report(QStringLiteral("Reference target is missing"));
            return;
        }
        if (!allowedTypes.isEmpty() && !allowedTypes.contains(typeByGuid.value(id)))
            report(QStringLiteral("Reference target has the wrong type"));
    };

    const auto valueAt = [](QJsonValue value, const QStringList& path) {
        for (const QString& segment : path) {
            if (!value.isObject()) return QJsonValue(QJsonValue::Undefined);
            const QJsonObject object = value.toObject();
            if (!object.contains(segment)) return QJsonValue(QJsonValue::Undefined);
            value = object.value(segment);
        }
        return value;
    };

    struct LocatedValue {
        QJsonValue value;
        QJsonObject parent;
        QString path;
    };
    const auto locatedValues = [](const QJsonObject& root,
                                  const QStringList& segments,
                                  const QString& rootPath) {
        QList<LocatedValue> values;
        std::function<void(const QJsonValue&, qsizetype, const QString&,
                           const QJsonObject&)> visit;
        visit = [&](const QJsonValue& current, qsizetype index,
                    const QString& path, const QJsonObject& parent) {
            if (index == segments.size()) {
                values.append({current, parent, path});
                return;
            }
            const QString& segment = segments.at(index);
            if (segment == QLatin1String("*")) {
                if (!current.isArray()) return;
                const QJsonArray array = current.toArray();
                for (qsizetype itemIndex = 0; itemIndex < array.size(); ++itemIndex) {
                    const QJsonValue item = array.at(itemIndex);
                    visit(item, index + 1,
                          path + QStringLiteral("[%1]").arg(itemIndex),
                          item.toObject());
                }
                return;
            }
            if (!current.isObject()) return;
            const QJsonObject object = current.toObject();
            if (!object.contains(segment)) return;
            visit(object.value(segment), index + 1,
                  path + QLatin1Char('.') + segment, object);
        };
        visit(root, 0, rootPath, root);
        return values;
    };

    ContractRuleExecutor executor;
    executor.uniqueAcrossProjectCheck = [&](const QStringList& fields) {
        const auto appendDuplicates = [&](const QMap<QString, QStringList>& paths,
                                           const QString& message) {
            for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
                QStringList values = it.value();
                std::sort(values.begin(), values.end());
                for (qsizetype i = 1; i < values.size(); ++i)
                    errors.append(QStringLiteral("%1 in %2").arg(message, values.at(i)));
            }
        };
        for (const QString& field : fields) {
            if (field == QLatin1String("guid"))
                appendDuplicates(guidPaths, QStringLiteral("Duplicate guid"));
            else if (field == QLatin1String("documentId"))
                appendDuplicates(documentIdPaths,
                                 QStringLiteral("Duplicate document identity"));
        }
    };
    executor.documentIdMirrorsRootCheck = [&](const QStringList& documentTypes) {
        for (const PackRecord& pack : packs) {
            if (!documentTypes.contains(pack.documentType)) continue;
            const QString contentGuid = QUuid::fromString(
                pack.object.value(QStringLiteral("guid")).toString())
                                            .toString(QUuid::WithoutBraces);
            if (contentGuid.isEmpty() || contentGuid != pack.documentId)
                errors.append(QStringLiteral("Document identity does not match its content in %1")
                                  .arg(pack.path));
        }
    };
    executor.projectIdUniformCheck = [&] {
        if (rootDocument.id != rootDocument.projectId)
            errors.append(QStringLiteral("Project root identity does not match its project identity"));
        for (const PackRecord& pack : packs) {
            if (pack.projectId != rootDocument.id)
                errors.append(QStringLiteral("Project identity does not match in %1")
                                  .arg(pack.path));
        }
    };
    executor.schemaVersionUniformCheck = [&] {
        for (const PackRecord& pack : packs) {
            if (pack.schemaVersion != schemaVersion)
                errors.append(QStringLiteral("Project Schema version does not match in %1")
                                  .arg(pack.path));
        }
    };
    executor.acyclicCheck = [&](const QStringList& referencePath) {
        QHash<QString, QString> layerParents;
        QHash<QString, QString> busParents;
        const auto addParent = [&](const QJsonObject& object,
                                   QHash<QString, QString>& parents,
                                   const QString& parentKind) {
            const QString id = QUuid::fromString(
                object.value(QStringLiteral("guid")).toString())
                                   .toString(QUuid::WithoutBraces);
            const QJsonObject parent = valueAt(object, referencePath).toObject();
            if (!id.isEmpty()
                && parent.value(QStringLiteral("kind")).toString() == parentKind) {
                parents.insert(
                    id,
                    QUuid::fromString(parent.value(QStringLiteral("id")).toString())
                        .toString(QUuid::WithoutBraces));
            }
        };
        for (const PackRecord& pack : packs) {
            if (pack.domain == QLatin1String("pack.layer"))
                addParent(pack.object, layerParents, QStringLiteral("item"));
            else if (pack.domain == QLatin1String("pack.bus"))
                addParent(pack.object, busParents, QStringLiteral("bus"));
        }
        for (const NodeRecord& node : nodes)
            addParent(node.object, layerParents, QStringLiteral("item"));
        rejectCycles(layerParents, QStringLiteral("Layer"));
        rejectCycles(busParents, QStringLiteral("Bus"));
    };
    executor.referencesResolveCheck = [&](const QList<ProjectSchemaReferenceRule>& references) {
        for (const ProjectSchemaReferenceRule& reference : references) {
            for (const PackRecord& pack : packs) {
                const QList<LocatedValue> values =
                    locatedValues(pack.object, reference.path, pack.path);
                for (const LocatedValue& located : values) {
                    if (reference.kind == ProjectSchemaReferenceKind::Parent) {
                        const QJsonObject parent = located.value.toObject();
                        const QString kind =
                            parent.value(QStringLiteral("kind")).toString();
                        if (kind == QLatin1String("root")) {
                            if (parent.contains(QStringLiteral("id"))
                                || parent.contains(QStringLiteral("documentId"))) {
                                errors.append(QStringLiteral("Root parent reference contains a target in %1")
                                                  .arg(located.path));
                            }
                            continue;
                        }
                        QStringList types;
                        if (kind == QLatin1String("bus"))
                            types.append(QStringLiteral("Bus"));
                        else if (kind == QLatin1String("item"))
                            types = {QStringLiteral("Sound"), QStringLiteral("Container"),
                                     QStringLiteral("Track"), schemaVersion < kCurrentProjectSchemaVersion
                                         ? QStringLiteral("Piece") : QStringLiteral("Segment"),
                                     QStringLiteral("Layer")};
                        validateReference(located.value, located.path, types);
                        continue;
                    }
                    QStringList targetTypes = reference.targetTypes;
                    if (!reference.selectorField.isEmpty()) {
                        targetTypes = reference.targetTypesBySelector.value(
                            located.parent.value(reference.selectorField).toString());
                    }
                    if (reference.kind == ProjectSchemaReferenceKind::Guid) {
                        const QString playbackKind = reference.path.contains(QStringLiteral("selections"))
                            && reference.path.back() == QLatin1String("childGuid")
                            ? QStringLiteral("Selection occurrence")
                            : reference.path.contains(QStringLiteral("tracktimeline"))
                                && reference.path.back() == QLatin1String("targetGuid")
                                ? QStringLiteral("Clip") : QString();
                        validateGuidReference(located.value, located.path, targetTypes,
                                              pack.object, playbackKind);
                    } else
                        validateReference(located.value, located.path, targetTypes,
                                          reference.allowNull);
                }
            }
        }

        QStringList parentPath;
        for (const ProjectSchemaReferenceRule& reference : references) {
            if (reference.kind == ProjectSchemaReferenceKind::Parent) {
                parentPath = reference.path;
                break;
            }
        }
        for (const PackRecord& pack : packs) {
            const QJsonObject parentRef = valueAt(pack.object, parentPath).toObject();
            const QString kind = parentRef.value(QStringLiteral("kind")).toString();
            if (pack.domain == QStringLiteral("pack.layer")) {
                if (kind != QStringLiteral("root") && kind != QStringLiteral("item")) {
                    errors.append(QStringLiteral("Layer pack parent reference is invalid in %1")
                                      .arg(pack.path));
                } else if (kind == QStringLiteral("item")) {
                    const QString parentGuid =
                        QUuid::fromString(parentRef.value(QStringLiteral("id")).toString())
                            .toString(QUuid::WithoutBraces);
                    if (parentGuid.isEmpty() || !guids.contains(parentGuid))
                        errors.append(QStringLiteral("Layer pack parent is missing in %1")
                                          .arg(pack.path));
                }
            } else if (pack.domain == QStringLiteral("pack.bus")) {
                if (kind != QStringLiteral("root") && kind != QStringLiteral("bus"))
                    errors.append(QStringLiteral("Bus pack parent reference is invalid in %1")
                                      .arg(pack.path));
                if (kind == QStringLiteral("bus")) {
                    const QString id = QUuid::fromString(
                        parentRef.value(QStringLiteral("id")).toString())
                                           .toString(QUuid::WithoutBraces);
                    if (id.isEmpty() || !guids.contains(id)
                        || typeByGuid.value(id) != QLatin1String("Bus"))
                        errors.append(QStringLiteral("Bus pack parent is missing in %1")
                                          .arg(pack.path));
                }
            } else {
                if (kind != QStringLiteral("root")) {
                    errors.append(QStringLiteral("Pack parent reference is invalid in %1")
                                      .arg(pack.path));
                }
            }
        }

        for (const NodeRecord& node : nodes) {
            const QJsonObject parentRef = valueAt(node.object, parentPath).toObject();
            const QString kind = parentRef.value(QStringLiteral("kind")).toString();
            if (node.layerRoot) {
                if (kind != QStringLiteral("root") && kind != QStringLiteral("item"))
                    errors.append(QStringLiteral("Layer root parent reference is invalid in %1")
                                      .arg(node.path));
            } else if (kind != QStringLiteral("item")) {
                errors.append(QStringLiteral("Layer item parent reference is invalid in %1")
                                  .arg(node.path));
            }
            if (kind == QStringLiteral("item")) {
                const QString parentGuid =
                    QUuid::fromString(parentRef.value(QStringLiteral("id")).toString())
                        .toString(QUuid::WithoutBraces);
                if (parentGuid.isEmpty() || !guids.contains(parentGuid))
                    errors.append(QStringLiteral("Layer item parent is missing in %1")
                                      .arg(node.path));
            }
        }
    };
    executor.singletonCheck = [&](const QString& documentType,
                                  const QStringList& predicatePath,
                                  const QString& expectedValue,
                                  const QString& message) {
        int matchingCount = 0;
        for (const PackRecord& pack : packs) {
            if (pack.documentType == documentType
                && valueAt(pack.object, predicatePath).toString() == expectedValue)
                ++matchingCount;
        }
        if (matchingCount != 1)
            errors.append(message);
    };

    QStringList ruleDispatchErrors;
    GeneratedSchemaRegistry::validateProjectRules(schemaVersion, executor,
                                                  ruleDispatchErrors);
    errors.append(ruleDispatchErrors);

    QDirIterator allEntries(projectDirectory,
                            QDir::AllEntries | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
    while (allEntries.hasNext()) {
        const QFileInfo fileInfo(allEntries.next());
        const QString relative = QDir::fromNativeSeparators(directory.relativeFilePath(fileInfo.absoluteFilePath()));
        if (relative.startsWith(QStringLiteral(".cache/folder-operations/"))) continue;
        if (fileInfo.isSymLink())
            errors.append(QStringLiteral("Project contains an unsupported symbolic link: %1")
                              .arg(fileInfo.absoluteFilePath()));
    }

    QDirIterator allFiles(projectDirectory,
                          QDir::Files | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (allFiles.hasNext()) {
        const QFileInfo fileInfo(allFiles.next());
        if (fileInfo.isSymLink()) continue;
        if (fileInfo.suffix().compare(QStringLiteral("vp"), Qt::CaseInsensitive) != 0)
            continue;
        const QString relative = QDir::fromNativeSeparators(directory.relativeFilePath(fileInfo.absoluteFilePath()));
        if (relative.startsWith(QStringLiteral(".cache/folder-operations/"))) continue;
        const QString topLevel = relative.section(QChar('/'), 0, 0)
                                      .section(QChar('\\'), 0, 0);
        if (domainForFolder(topLevel).isEmpty())
            errors.append(QStringLiteral("Pack file is outside a managed domain: %1")
                              .arg(relative));
    }

    QMap<QString, QJsonObject> platformDocuments;
    for (const auto& pack : packs)
        platformDocuments.insert(pack.path, pack.object);
    if (schemaVersion == ProjectSchema::kProjectSchemaVersion)
        errors.append(ProjectPlatformSemantics::validateDocuments(projectRoot, platformDocuments));

    if (includeVirtualDocuments && context) {
        context->parsed_.clear();
        context->validated_.removeIf([&](const auto& entry) { return !validatedPaths.contains(entry.key()); });
        context->decoded_.removeIf([&](const auto& entry) { return !validatedPaths.contains(entry.key()); });
    }
    return errors;
}

}
