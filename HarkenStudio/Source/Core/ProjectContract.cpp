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
#include <utility>

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

QString packDomain(const QJsonObject& object)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type.compare(QStringLiteral("Sound"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Container"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Track"), Qt::CaseInsensitive) == 0
        || type.compare(QStringLiteral("Piece"), Qt::CaseInsensitive) == 0
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

QJsonValue valueAt(QJsonValue value, const QStringList& path)
{
    for (const QString& segment : path) {
        if (!value.isObject()) return QJsonValue(QJsonValue::Undefined);
        const QJsonObject object = value.toObject();
        if (!object.contains(segment)) return QJsonValue(QJsonValue::Undefined);
        value = object.value(segment);
    }
    return value;
}

struct LocatedValue
{
    QJsonValue value;
    QJsonObject parent;
    QString path;
};

QList<LocatedValue> locatedValues(const QJsonObject& root,
                                  const QStringList& segments,
                                  const QString& rootPath)
{
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
}

QJsonObject rootSemanticProjection(const QJsonObject& document)
{
    QJsonObject result = document;
    QJsonObject content = result.value(QStringLiteral("content")).toObject();
    QJsonObject metadata = content.value(QStringLiteral("metadata")).toObject();
    metadata.remove(QStringLiteral("modifiedDate"));
    content.insert(QStringLiteral("metadata"), metadata);
    result.insert(QStringLiteral("content"), content);
    return result;
}

QJsonArray documentSemanticProjection(const QJsonObject& document, int schemaVersion,
                                      QList<ProjectSchemaReferenceRule>* referenceRules = nullptr)
{
    const QJsonObject header = document.value(QStringLiteral("harkenDocument")).toObject();
    const QJsonObject content = document.value(QStringLiteral("content")).toObject();
    const QJsonObject components = content.value(QStringLiteral("components")).toObject();
    QJsonArray result{header, content.value(QStringLiteral("guid")),
        content.value(QStringLiteral("type")), content.value(QStringLiteral("name")),
        content.value(QStringLiteral("children")), content.value(QStringLiteral("fieldGroups")),
        components.value(QStringLiteral("selection")),
        components.value(QStringLiteral("tracktimeline")),
        components.value(QStringLiteral("action")),
        components.value(QStringLiteral("mixnode")).toObject().value(QStringLiteral("bindings"))};
    ContractRuleExecutor executor;
    executor.uniqueAcrossProjectCheck = [&](const QStringList& fields) {
        for (const QString& field : fields)
            result.append(field == QLatin1String("documentId")
                ? header.value(QStringLiteral("id")) : content.value(field));
    };
    executor.documentIdMirrorsRootCheck = [&](const QStringList&) {
        result.append(header.value(QStringLiteral("id")));
        result.append(content.value(QStringLiteral("guid")));
    };
    executor.projectIdUniformCheck = [&] {
        result.append(header.value(QStringLiteral("projectId")));
    };
    executor.referencesResolveCheck = [&](const QList<ProjectSchemaReferenceRule>& references) {
        if (referenceRules) *referenceRules = references;
        for (const ProjectSchemaReferenceRule& reference : references) {
            QJsonArray values;
            for (const LocatedValue& located : locatedValues(content, reference.path, {})) {
                QJsonArray entry{located.path, located.value};
                if (!reference.selectorField.isEmpty())
                    entry.append(located.parent.value(reference.selectorField));
                values.append(entry);
            }
            result.append(values);
        }
    };
    executor.acyclicCheck = [&](const QStringList& path) {
        result.append(valueAt(content, path));
    };
    executor.singletonCheck = [&](const QString&, const QStringList& path,
                                  const QString&, const QString&) {
        result.append(valueAt(content, path));
    };
    executor.schemaVersionUniformCheck = [&] {
        result.append(header.value(QStringLiteral("schemaVersion")));
    };
    QStringList errors;
    if (!GeneratedSchemaRegistry::validateProjectRules(schemaVersion, executor, errors)
        || !errors.isEmpty())
        return {};
    return result;
}

QSet<QString> referencedDocumentPaths(const QJsonObject& document, int schemaVersion,
                                     const QHash<QString, QString>& pathsByGuid,
                                     const QString& rootBusPath)
{
    QSet<QString> result;
    const auto addReference = [&](const QJsonValue& value) {
        const QUuid guid = QUuid::fromString(value.isObject()
            ? value.toObject().value(QStringLiteral("id")).toString() : value.toString());
        if (guid.isNull()) return;
        const auto path = pathsByGuid.constFind(guid.toString(QUuid::WithoutBraces));
        if (path != pathsByGuid.cend()) result.insert(path.value());
    };
    const QJsonObject content = document.value(QStringLiteral("content")).toObject();
    QList<ProjectSchemaReferenceRule> referenceRules;
    documentSemanticProjection(document, schemaVersion, &referenceRules);
    for (const auto& rule : referenceRules)
        for (const auto& value : locatedValues(content, rule.path, {}))
            addReference(value.value);

    const QJsonObject groups = content.value(QStringLiteral("fieldGroups")).toObject();
    const auto addGroupReferences = [&](const QJsonObject& scope) {
        addReference(valueAt(scope, {QStringLiteral("mainOutput"), QStringLiteral("values"),
                                     QStringLiteral("mainOutputBusRef")}));
        addReference(valueAt(scope, {QStringLiteral("spatialRating"), QStringLiteral("values"),
                                     QStringLiteral("spatialAttenuationRef")}));
        for (const auto& row : valueAt(scope, {QStringLiteral("send"), QStringLiteral("values"),
                                              QStringLiteral("sends")}).toArray())
            addReference(row.toObject().value(QStringLiteral("targetBusID")));
    };
    if (!groups.isEmpty()) {
        if (!rootBusPath.isEmpty()) result.insert(rootBusPath);
        addGroupReferences(groups.value(QStringLiteral("shared")).toObject());
        const QJsonObject platforms = groups.value(QStringLiteral("platforms")).toObject();
        for (auto scope = platforms.constBegin(); scope != platforms.constEnd(); ++scope)
            addGroupReferences(scope.value().toObject());
    }

    const QJsonObject components = content.value(QStringLiteral("components")).toObject();
    const QJsonObject selection = components.value(QStringLiteral("selection")).toObject();
    const QJsonObject selections = selection.value(QStringLiteral("selections")).toObject();
    const auto addSelectionReferences = [&](const QJsonObject& table) {
        for (const auto& axis : table.value(QStringLiteral("matchAxes")).toArray()) addReference(axis);
        for (const auto& axis : table.value(QStringLiteral("blendAxes")).toArray())
            addReference(axis.isObject() ? axis.toObject().value(QStringLiteral("variable")) : axis);
        for (const auto& row : table.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject entry = row.toObject();
            addReference(entry.value(QStringLiteral("childGuid")));
            for (const auto& curve : entry.value(QStringLiteral("curves")).toArray())
                addReference(curve.toObject().value(QStringLiteral("variable")));
        }
    };
    addSelectionReferences(selections.value(QStringLiteral("shared")).toObject());
    const QJsonObject selectionPlatforms = selections.value(QStringLiteral("platforms")).toObject();
    for (auto table = selectionPlatforms.constBegin(); table != selectionPlatforms.constEnd(); ++table)
        addSelectionReferences(table.value().toObject());
    for (const auto& rule : selection.value(QStringLiteral("transitions")).toObject()
             .value(QStringLiteral("rules")).toArray()) {
        const QJsonObject transition = rule.toObject();
        for (const auto* field : {"source", "target", "bridge"})
            addReference(transition.value(QLatin1String(field)));
    }
    for (const auto& binding : components.value(QStringLiteral("mixnode")).toObject()
             .value(QStringLiteral("bindings")).toArray())
        addReference(binding.toObject().value(QStringLiteral("variable")));
    for (const auto& step : components.value(QStringLiteral("action")).toObject()
             .value(QStringLiteral("steps")).toArray()) {
        addReference(step.toObject().value(QStringLiteral("targetRef")));
        addReference(valueAt(step, {QStringLiteral("quantize"), QStringLiteral("referenceRef")}));
    }
    return result;
}

QSet<QString> inheritedDocumentPaths(const QSet<QString>& references)
{
    QSet<QString> result;
    for (const QString& path : references) {
        const QString domain = domainForFolder(path.section(QLatin1Char('/'), 0, 0));
        if (domain == QLatin1String("pack.bus") || domain == QLatin1String("pack.attenuation")
            || domain == QLatin1String("pack.variable"))
            result.insert(path);
    }
    return result;
}

}

void ProjectContractValidationBaseline::clear()
{
    *this = {};
}

QStringList ProjectContractValidationBaseline::dependencyPaths(
    const QStringList& changedPaths) const
{
    if (!valid_ || changedPaths.isEmpty()) return {};
    QSet<QString> paths{rootRelativePath_};
    QSet<QString> ancestors;
    for (const QString& path : changedPaths) {
        paths.insert(path);
        paths.unite(dependencies_.value(path));
        QString parent = parentPaths_.value(path);
        while (!parent.isEmpty() && !ancestors.contains(parent)) {
            ancestors.insert(parent);
            paths.insert(parent);
            paths.unite(inheritedDependencies_.value(parent));
            parent = parentPaths_.value(parent);
        }
    }
    QStringList result = paths.values();
    result.sort();
    return result;
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
    ProjectContractReadContext* context,
    ProjectContractValidationBaseline* baseline,
    const QString& rootRelativePath) const
{
    return validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
                            overrides, context, false, baseline, true, false, rootRelativePath);
}

QStringList ProjectContractRegistry::validateSnapshot(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema, const QHash<QString, QJsonObject>& documents,
    ProjectContractReadContext* context,
    ProjectContractValidationBaseline* baseline,
    const QString& rootRelativePath, bool includePhysicalDocuments) const
{
    return validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
                            documents, context, true, baseline, includePhysicalDocuments,
                            false, rootRelativePath);
}

QStringList ProjectContractRegistry::validateIncrementalSnapshot(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema, const QHash<QString, QJsonObject>& changedDocuments,
    const QSet<QString>& removedPaths,
    const ProjectContractValidationBaseline& baseline,
    ProjectContractValidationBaseline* validatedBaseline,
    ProjectContractReadContext* context,
    ProjectContractValidationDetails* details) const
{
    if (details) *details = {};
    if (!baseline.isValid()) {
        if (validatedBaseline) validatedBaseline->clear();
        return {QStringLiteral("Incremental validation requires a validated project baseline")};
    }
    QStringList errors;
    const auto validatePath = [&](const QString& path) {
        if (!ProjectDataPolicy::isSafeRelativePath(path)
            || ProjectDataPolicy::normalizeRelativePath(path) != path
            || QFileInfo(path).suffix().compare(QStringLiteral("vp"), Qt::CaseInsensitive) != 0
            || domainForFolder(path.section(QLatin1Char('/'), 0, 0)).isEmpty())
            errors.append(QStringLiteral("Invalid project document path: %1").arg(path));
    };
    for (const QString& path : removedPaths) {
        validatePath(path);
        if (changedDocuments.contains(path))
            errors.append(QStringLiteral("Project document is both changed and removed: %1").arg(path));
    }
    bool reuseGlobalValidation = removedPaths.isEmpty()
        && baseline.requireCurrentSchema_ == requireCurrentSchema
        && rootSemanticProjection(projectRootDocument)
            == rootSemanticProjection(baseline.rootDocument_);
    for (auto document = changedDocuments.cbegin(); document != changedDocuments.cend(); ++document) {
        validatePath(document.key());
        if (!reuseGlobalValidation) continue;
        const auto previous = baseline.semanticProjections_.constFind(document.key());
        const QJsonArray projection = documentSemanticProjection(document.value(), baseline.schemaVersion_);
        if (previous == baseline.semanticProjections_.cend()
            || projection.isEmpty() || previous.value() != projection)
            reuseGlobalValidation = false;
    }
    if (!errors.isEmpty()) {
        if (validatedBaseline) validatedBaseline->clear();
        return errors;
    }

    ProjectContractValidationBaseline candidate;
    if (reuseGlobalValidation) {
        errors = validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
            changedDocuments, context, true, nullptr, false, true, baseline.rootRelativePath_);
        if (errors.isEmpty() && validatedBaseline) {
            candidate = baseline;
            candidate.rootDocument_ = projectRootDocument;
            for (auto document = changedDocuments.cbegin(); document != changedDocuments.cend(); ++document) {
                candidate.documents_.insert(document.key(), document.value());
            }
        }
        if (details) details->readPaths = baseline.dependencyPaths(changedDocuments.keys());
    } else {
        QHash<QString, QJsonObject> documents = baseline.documents_;
        for (const QString& path : removedPaths) documents.remove(path);
        for (auto document = changedDocuments.cbegin(); document != changedDocuments.cend(); ++document)
            documents.insert(document.key(), document.value());
        errors = validateTreeImpl(projectDirectory, projectRootDocument, requireCurrentSchema,
            documents, context, true, validatedBaseline ? &candidate : nullptr,
            false, false, baseline.rootRelativePath_);
        if (details) {
            details->fullValidation = true;
            QSet<QString> paths{baseline.rootRelativePath_};
            for (auto document = baseline.documents_.cbegin(); document != baseline.documents_.cend(); ++document)
                paths.insert(document.key());
            for (auto document = documents.cbegin(); document != documents.cend(); ++document)
                paths.insert(document.key());
            details->readPaths = paths.values();
            details->readPaths.sort();
        }
    }
    if (validatedBaseline) *validatedBaseline = std::move(candidate);
    return errors;
}

QStringList ProjectContractRegistry::validateTreeImpl(
    const QString& projectDirectory, const QJsonObject& projectRootDocument,
    bool requireCurrentSchema, const QHash<QString, QJsonObject>& overrides,
    ProjectContractReadContext* context, bool includeVirtualDocuments,
    ProjectContractValidationBaseline* baseline, bool includePhysicalDocuments,
    bool reuseGlobalValidation, const QString& knownRootRelativePath) const
{
    QStringList errors;
    const QDir directory(projectDirectory);
    QString rootRelativePath = knownRootRelativePath;
    if (rootRelativePath.isEmpty()) {
        const QStringList projectFiles = directory.entryList(
            {QStringLiteral("*.hsp")}, QDir::Files, QDir::Name);
        rootRelativePath = projectFiles.isEmpty()
            ? directory.dirName() + QStringLiteral(".hsp") : projectFiles.first();
    }
    ProjectDocumentData rootDocument;
    QString rootDocumentError;
    if (!decodeDocument(projectRootDocument, QStringLiteral("project.root"),
                         rootRelativePath, &rootDocument, &rootDocumentError, context)) {
        errors.append(rootDocumentError);
        if (includeVirtualDocuments && context) context->clear();
        if (baseline) baseline->clear();
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
    QHash<QString, QJsonObject> validatedDocuments;

    const auto validateComponents = [&](const QJsonObject& object,
                                        const QString& path) {
        if (!object.value(QStringLiteral("components")).isObject()) {
            errors.append(QStringLiteral("Components are malformed in %1").arg(path));
            return;
        }
        const QJsonObject components = object.value(QStringLiteral("components"))
                                            .toObject();
        for (auto it = components.constBegin(); it != components.constEnd(); ++it) {
            if (!ComponentCatalog::parseComponentName(it.key()).has_value())
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
        const QString nodeDomain = packDomain(object);
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
        if (virtualPaths.value(folder).isEmpty()
            && (!includePhysicalDocuments || !packDirectory.exists()))
            continue;
        QStringList paths;
        if (includePhysicalDocuments) {
            QDirIterator files(packDirectory.absolutePath(),
                               {QStringLiteral("*.vp")},
                               QDir::Files | QDir::Readable,
                               QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const QFileInfo fileInfo(files.next());
                paths.append(QDir::fromNativeSeparators(
                    directory.relativeFilePath(fileInfo.absoluteFilePath())));
            }
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
            if (baseline) validatedDocuments.insert(path, packDocument);
            documentIdPaths[document.id].append(path);
            const QJsonObject pack = document.content;
            const QString contentGuid = QUuid::fromString(
                pack.value(QStringLiteral("guid")).toString())
                                            .toString(QUuid::WithoutBraces);
            const QString domain = packDomain(pack);
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
                                     QStringLiteral("Track"), QStringLiteral("Piece"),
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

    if (!reuseGlobalValidation) {
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
    }

    QMap<QString, QJsonObject> platformDocuments;
    for (const auto& pack : packs)
        platformDocuments.insert(pack.path, pack.object);
    if (schemaVersion == ProjectSchema::kProjectSchemaVersion)
        errors.append(ProjectPlatformSemantics::validateDocuments(projectRoot, platformDocuments));

    if (includeVirtualDocuments && context && !reuseGlobalValidation) {
        context->parsed_.clear();
        context->validated_.removeIf([&](const auto& entry) { return !validatedPaths.contains(entry.key()); });
        context->decoded_.removeIf([&](const auto& entry) { return !validatedPaths.contains(entry.key()); });
    }
    ProjectContractValidationBaseline nextBaseline;
    if (baseline && errors.isEmpty()) {
        nextBaseline.valid_ = true;
        nextBaseline.requireCurrentSchema_ = requireCurrentSchema;
        nextBaseline.schemaVersion_ = schemaVersion;
        nextBaseline.rootRelativePath_ = rootRelativePath;
        nextBaseline.rootDocument_ = projectRootDocument;
        nextBaseline.documents_ = std::move(validatedDocuments);
        for (auto document = nextBaseline.documents_.cbegin(); document != nextBaseline.documents_.cend(); ++document) {
            const QJsonObject content = document.value().value(QStringLiteral("content")).toObject();
            const QString guid = QUuid::fromString(content.value(QStringLiteral("guid")).toString())
                .toString(QUuid::WithoutBraces);
            nextBaseline.pathsByGuid_.insert(guid, document.key());
            nextBaseline.semanticProjections_.insert(document.key(),
                documentSemanticProjection(document.value(), schemaVersion));
        }
        QString rootBusPath;
        for (const auto& pack : packs)
            if (pack.domain == QLatin1String("pack.bus")
                && pack.object.value(QStringLiteral("parentRef")).toObject()
                    .value(QStringLiteral("kind")).toString() == QLatin1String("root"))
                rootBusPath = pack.path;
        for (auto document = nextBaseline.documents_.cbegin(); document != nextBaseline.documents_.cend(); ++document) {
            const auto dependencies = referencedDocumentPaths(document.value(), schemaVersion,
                nextBaseline.pathsByGuid_, rootBusPath);
            nextBaseline.dependencies_.insert(document.key(), dependencies);
            nextBaseline.inheritedDependencies_.insert(document.key(), inheritedDocumentPaths(dependencies));
            const QString parentGuid = QUuid::fromString(valueAt(document.value(),
                {QStringLiteral("content"), QStringLiteral("parentRef"), QStringLiteral("id")}).toString())
                .toString(QUuid::WithoutBraces);
            nextBaseline.parentPaths_.insert(document.key(), nextBaseline.pathsByGuid_.value(parentGuid));
        }
    }
    if (baseline) *baseline = std::move(nextBaseline);
    return errors;
}

}
