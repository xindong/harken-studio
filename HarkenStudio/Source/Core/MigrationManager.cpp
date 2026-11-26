
#include "MigrationManager.h"
#include "ConversionMigration.h"
#include "ItemTypeCatalog.h"
#include "MixingSemantics.h"
#include "ProjectDataPolicy.h"
#include "ProjectDocument.h"
#include "ProjectContract.h"
#include "ProjectTypes.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace harken::studio {
namespace {

QStringList projectDocumentRelativePaths(const QString& projectDirectory,
                                         const QString& projectFile)
{
    const ProjectDataPolicy dataPolicy = ProjectDataPolicy::forProjectFile(projectFile);
    const QDir root(projectDirectory);
    QStringList paths{ProjectDataPolicy::normalizeRelativePath(
        root.relativeFilePath(projectFile))};
    for (const auto bucket : ItemTypeCatalog::allBuckets()) {
        QDirIterator files(root.filePath(ItemTypeCatalog::folderNameOf(bucket)),
                           {QStringLiteral("*.vp")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString relative = ProjectDataPolicy::normalizeRelativePath(
                root.relativeFilePath(files.next()));
            if (dataPolicy.classify(relative) == ProjectDataClass::Raw)
                paths.append(relative);
        }
    }
    paths.sort();
    paths.removeDuplicates();
    return paths;
}

void expandWriteSetPlaceholders(QStringList* writeSet,
                                const QString& projectDirectory,
                                const QString& projectFile)
{
    if (!writeSet)
        return;
    const QString relative = ProjectDataPolicy::normalizeRelativePath(
        QDir(projectDirectory).relativeFilePath(projectFile));
    QStringList expanded;
    bool sawAllDocuments = false;
    for (const QString& path : *writeSet) {
        if (path == QStringLiteral("<project-file>")) {
            if (!relative.isEmpty())
                expanded.append(relative);
        } else if (path == QStringLiteral("<all-documents>")) {
            sawAllDocuments = true;
        } else {
            expanded.append(path);
        }
    }
    if (sawAllDocuments)
        expanded.append(projectDocumentRelativePaths(projectDirectory, projectFile));
    expanded.sort();
    expanded.removeDuplicates();
    *writeSet = expanded;
}

void editObject(QJsonObject& owner, const QString& key,
                const std::function<void(QJsonObject&)>& edit)
{
    if (!owner.value(key).isObject()) return;
    auto object = owner.value(key).toObject();
    edit(object);
    owner.insert(key, object);
}

void editObjects(QJsonObject& owner, const QString& key,
                 const std::function<void(QJsonObject&)>& edit)
{
    if (!owner.value(key).isArray()) return;
    auto array = owner.value(key).toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
        auto object = array[index].toObject();
        edit(object);
        array[index] = object;
    }
    owner.insert(key, array);
}

class LabelReferences
{
public:
    explicit LabelReferences(const QMap<QString, QJsonObject>& documents)
    {
        for (auto document = documents.cbegin(); document != documents.cend(); ++document) {
            const auto content = document->value(QStringLiteral("content")).toObject();
            if (content.value(QStringLiteral("type")) != QStringLiteral("Variable")) continue;
            variables_.insert(QUuid(content.value(QStringLiteral("guid")).toString()),
                {document.key(), content.value(QStringLiteral("components")).toObject()
                    .value(QStringLiteral("variable")).toObject().value(QStringLiteral("labels")).toArray()});
        }
    }

    QString resolve(const QUuid& variableId, int anchor)
    {
        const auto variable = variables_.find(variableId);
        if (variable == variables_.end()) return {};
        auto& labels = variable->labels;
        qsizetype match = -1;
        for (qsizetype index = 0; index < labels.size(); ++index) {
            if (labels[index].toObject().value(QStringLiteral("anchor")).toInt() != anchor) continue;
            if (match >= 0) return {};
            match = index;
        }
        if (match < 0) {
            match = labels.size();
            labels.append(QJsonObject{{QStringLiteral("anchor"), anchor}, {QStringLiteral("name"), QString{}}});
        }
        auto label = labels[match].toObject();
        if (!label.contains(QStringLiteral("id"))) {
            label.insert(QStringLiteral("id"), QUuid::createUuidV5(variableId,
                QByteArray("grid-label-") + QByteArray::number(anchor)).toString(QUuid::WithoutBraces));
            labels[match] = label;
            variable->changed = true;
        }
        return label.value(QStringLiteral("id")).toString();
    }

    void writeTo(QMap<QString, QJsonObject>& documents) const
    {
        for (const auto& variable : variables_) {
            if (!variable.changed) continue;
            editObject(documents[variable.document], QStringLiteral("content"), [&](QJsonObject& content) {
                editObject(content, QStringLiteral("components"), [&](QJsonObject& components) {
                    editObject(components, QStringLiteral("variable"), [&](QJsonObject& component) {
                        component.insert(QStringLiteral("labels"), variable.labels);
                    });
                });
            });
        }
    }

private:
    struct Variable
    {
        QString document;
        QJsonArray labels;
        bool changed = false;
    };
    QMap<QUuid, Variable> variables_;
};

bool migrateBlendRows(QJsonObject& table, const QJsonObject& content, QString* error)
{
    QMap<QUuid, QStringList> rowsByVariable;
    auto rows = table.value(QStringLiteral("blendAxes")).toArray();
    for (qsizetype index = 0; index < rows.size(); ++index) {
        if (!rows[index].isObject()) {
            const QUuid variable(rows[index].toString());
            rows[index] = QJsonObject{
                {QStringLiteral("guid"), MixingSemantics::legacyRowGuid(variable)
                    .toString(QUuid::WithoutBraces)},
                {QStringLiteral("variable"), rows[index].isNull()
                    ? QJsonValue(QJsonValue::Null) : QJsonValue(rows[index].toString())}};
        }
        const auto row = rows[index].toObject();
        rowsByVariable[QUuid(row.value(QStringLiteral("variable")).toString())]
            .append(row.value(QStringLiteral("guid")).toString());
    }
    table.insert(QStringLiteral("blendAxes"), rows);
    bool valid = true;
    editObjects(table, QStringLiteral("entries"), [&](QJsonObject& entry) {
        editObjects(entry, QStringLiteral("curves"), [&](QJsonObject& curve) {
            if (!curve.contains(QStringLiteral("variable"))) return;
            const auto matches = rowsByVariable.value(QUuid(curve.take(QStringLiteral("variable")).toString()));
            valid = valid && matches.size() == 1;
            if (matches.size() == 1) curve.insert(QStringLiteral("blendGuid"), matches.first());
        });
    });
    if (!valid && error)
        *error = QStringLiteral("Container '%1' has a Blend curve whose Variable row is missing or ambiguous")
            .arg(content.value(QStringLiteral("name")).toString());
    return valid;
}

bool migrateMatchRows(QJsonObject& table, const QJsonObject& content,
                      LabelReferences& labels, QString* error)
{
    auto matchRows = table.value(QStringLiteral("matchRows")).toArray();
    if (!table.contains(QStringLiteral("matchRows"))) {
        QVector<QPair<qint64, QJsonObject>> ordered;
        editObjects(table, QStringLiteral("entries"), [&](QJsonObject& entry) {
            for (const auto& value : entry.take(QStringLiteral("conditions")).toArray()) {
                auto row = value.toObject();
                const qint64 priority = row.take(QStringLiteral("priority")).toInteger();
                row.insert(QStringLiteral("target"), entry.value(QStringLiteral("childGuid")));
                ordered.append({priority, row});
            }
        });
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
        for (const auto& entry : ordered) matchRows.append(entry.second);
    }
    const auto axes = table.value(QStringLiteral("matchAxes")).toArray();
    for (qsizetype index = 0; index < matchRows.size(); ++index) {
        auto row = matchRows[index].toObject();
        auto cells = row.value(QStringLiteral("cells")).toArray();
        for (qsizetype axis = 0; axis < cells.size(); ++axis) {
            if (!cells[axis].isDouble()) continue;
            const auto anchor = cells[axis].toInt(-1);
            const auto label = axis < axes.size() && anchor >= 0
                ? labels.resolve(QUuid(axes[axis].toString()), anchor) : QString{};
            if (label.isEmpty()) {
                if (error)
                    *error = QStringLiteral("Container '%1' Match rule %2 column %3 value %4 has no unique Label")
                        .arg(content.value(QStringLiteral("name")).toString())
                        .arg(index + 1).arg(axis + 1).arg(anchor);
                return false;
            }
            cells[axis] = QJsonObject{{QStringLiteral("labelId"), label}};
        }
        row.insert(QStringLiteral("cells"), cells);
        matchRows[index] = row;
    }
    table.insert(QStringLiteral("matchRows"), matchRows);
    return true;
}

bool migrateSelection(QJsonObject& selection, const QJsonObject& content,
                      LabelReferences& labels, QString* error)
{
    bool valid = true;
    const auto migrateTable = [&](QJsonObject& table) {
        valid = valid && migrateBlendRows(table, content, error)
            && migrateMatchRows(table, content, labels, error);
    };
    editObject(selection, QStringLiteral("selections"), [&](QJsonObject& selections) {
        editObject(selections, QStringLiteral("shared"), migrateTable);
        editObject(selections, QStringLiteral("platforms"), [&](QJsonObject& platforms) {
            for (const auto& id : platforms.keys()) editObject(platforms, id, migrateTable);
        });
    });
    return valid;
}

bool migrateLegacyContent(QJsonObject* content, LabelReferences& labels, QString* error)
{
    auto components = content->value(QStringLiteral("components")).toObject();
    auto mix = components.value(QStringLiteral("mixnode")).toObject();
    if (mix.value(QStringLiteral("effects")).isArray()) {
        auto effects = mix.value(QStringLiteral("effects")).toArray();
        for (qsizetype index = 0; index < effects.size(); ++index) {
            auto effect = effects[index].toObject();
            if (!effect.contains(QStringLiteral("type"))) continue;
            if (effect.contains(QStringLiteral("mode")) || effect.contains(QStringLiteral("custom"))) {
                if (error) *error = QStringLiteral("Conflicting legacy Effect fields");
                return false;
            }
            effect.insert(QStringLiteral("custom"), QJsonObject{
                {QStringLiteral("type"), effect.take(QStringLiteral("type"))},
                {QStringLiteral("parameters"), effect.take(QStringLiteral("parameters"))}});
            effect.insert(QStringLiteral("mode"), QStringLiteral("custom"));
            effects[index] = effect;
        }
        mix.insert(QStringLiteral("effects"), effects);
    }
    if (content->value(QStringLiteral("fieldGroups")).isObject()) {
        auto groups = content->value(QStringLiteral("fieldGroups")).toObject();
        const auto migrateOutput = [](QJsonObject* scope) {
            if (!scope->contains(QStringLiteral("mainOutput"))) return;
            auto output = scope->take(QStringLiteral("mainOutput")).toObject();
            if (output.value(QStringLiteral("values")).isObject()) {
                auto values = output.value(QStringLiteral("values")).toObject();
                if (values.contains(QStringLiteral("mainOutputBusRef")))
                    values.insert(QStringLiteral("outputBusRef"),
                                  values.take(QStringLiteral("mainOutputBusRef")));
                output.insert(QStringLiteral("values"), values);
            }
            scope->insert(QStringLiteral("output"), output);
        };
        auto shared = groups.value(QStringLiteral("shared")).toObject();
        migrateOutput(&shared);
        if (!shared.contains(QStringLiteral("effect")))
            shared.insert(QStringLiteral("effect"), QJsonObject{
                {QStringLiteral("inherit"), false},
                {QStringLiteral("values"), QJsonObject{
                    {QStringLiteral("effects"), mix.take(QStringLiteral("effects")).toArray()}}}});
        groups.insert(QStringLiteral("shared"), shared);
        if (groups.value(QStringLiteral("platforms")).isObject()) {
            auto platforms = groups.value(QStringLiteral("platforms")).toObject();
            for (const auto& platform : platforms.keys()) {
                auto scope = platforms.value(platform).toObject();
                migrateOutput(&scope);
                platforms.insert(platform, scope);
            }
            groups.insert(QStringLiteral("platforms"), platforms);
        }
        content->insert(QStringLiteral("fieldGroups"), groups);
    }
    if (components.contains(QStringLiteral("mixnode")))
        components.insert(QStringLiteral("mixnode"), mix);
    if (components.contains(QStringLiteral("selection"))) {
        auto selection = components.value(QStringLiteral("selection")).toObject();
        if (!migrateSelection(selection, *content, labels, error)) return false;
        components.insert(QStringLiteral("selection"), selection);
    }
    if (content->contains(QStringLiteral("components")))
        content->insert(QStringLiteral("components"), components);
    if (content->value(QStringLiteral("type")) == QStringLiteral("Bus")) {
        auto properties = content->value(QStringLiteral("playableProperties")).toObject();
        const auto migrateReference = [](QJsonObject* values) {
            if (values->contains(QStringLiteral("mainOutputBusRef")))
                values->insert(QStringLiteral("outputBusRef"),
                               values->take(QStringLiteral("mainOutputBusRef")));
        };
        migrateReference(&properties);
        if (properties.value(QStringLiteral("platformOverrides")).isArray()) {
            auto overlays = properties.value(QStringLiteral("platformOverrides")).toArray();
            for (qsizetype index = 0; index < overlays.size(); ++index) {
                auto overlay = overlays[index].toObject();
                migrateReference(&overlay);
                overlays[index] = overlay;
            }
            properties.insert(QStringLiteral("platformOverrides"), overlays);
        }
        content->insert(QStringLiteral("playableProperties"), properties);
    }
    if (content->value(QStringLiteral("children")).isArray()) {
        auto children = content->value(QStringLiteral("children")).toArray();
        for (qsizetype index = 0; index < children.size(); ++index) {
            auto child = children[index].toObject();
            if (!migrateLegacyContent(&child, labels, error)) return false;
            children[index] = child;
        }
        content->insert(QStringLiteral("children"), children);
    }
    return true;
}

}

MigrationManager::MigrationManager()
{
    for (const int sourceSchema : {7, 8, 9}) {
        const int targetSchema = sourceSchema + 1;
        ProjectMigrationStep step;
        step.id = QStringLiteral("project-schema-%1-to-%2")
                      .arg(sourceSchema).arg(targetSchema);
        step.fromSchema = sourceSchema;
        step.toSchema = targetSchema;
        step.writeSet = {QStringLiteral("<all-documents>")};
        step.transform = [targetSchema](ProjectMigrationWorkspace& workspace,
                                       QStringList*, QString* error) {
            const auto paths = projectDocumentRelativePaths(workspace.sourceDirectory(),
                QDir(workspace.sourceDirectory()).filePath(workspace.projectFile()));
            for (const QString& relative : paths) {
                QJsonObject document;
                if (!workspace.readObject(relative, &document, error))
                    return false;
                QJsonObject header = document.value(QStringLiteral("harkenDocument")).toObject();
                header.insert(QStringLiteral("schemaVersion"), targetSchema);
                document.insert(QStringLiteral("harkenDocument"), header);
                if (!workspace.writeObject(relative, document, error))
                    return false;
            }
            return true;
        };
        registry_.registerStep(std::move(step));
    }
}

MigrationManager::MigrationManager(ProjectMigrationRegistry registry)
    : registry_(std::move(registry))
{
}

MigrationManager& MigrationManager::instance()
{
    static MigrationManager inst;
    return inst;
}

int MigrationManager::detectVersion(const QJsonObject& root) const
{
    return root.value(QStringLiteral("harkenDocument")).toObject()
        .value(QStringLiteral("schemaVersion")).toInt(0);
}

bool MigrationManager::registerMigration(ProjectMigrationStep step, QString* error)
{
    return registry_.registerStep(std::move(step), error);
}

bool MigrationManager::validateSchema(const QJsonObject& root,
                                      ProjectData& project)
{
    Q_UNUSED(project)
    const int version = detectVersion(root);

    if (version != CURRENT_PROJECT_SCHEMA_VERSION) {
        qWarning() << "[MigrationManager] Project Schema" << version
                   << "does not match the required HarkenStudio Schema"
                   << CURRENT_PROJECT_SCHEMA_VERSION << "- refusing to open.";
        return false;
    }

    return true;
}

ProjectMigrationPlan MigrationManager::planProject(
    const QString& projectDirectory, const QString& projectFile,
    int sourceSchema, int targetSchema) const
{
    ProjectMigrationPlan plan = registry_.plan(sourceSchema, targetSchema,
                                                projectDirectory);
    if (!plan.ok)
        return plan;
    expandWriteSetPlaceholders(&plan.writeSet, projectDirectory, projectFile);
    for (ProjectMigrationStep& step : plan.steps)
        expandWriteSetPlaceholders(&step.writeSet, projectDirectory, projectFile);
    for (ProjectMigrationStep& step : plan.steps) {
        if (step.id != QStringLiteral("project-schema-9-to-10")) continue;
        QMap<QString, QJsonObject> documents;
        for (const auto& relative : step.writeSet) {
            QFile file(QDir(projectDirectory).filePath(relative));
            if (!file.open(QIODevice::ReadOnly)) {
                plan.ok = false;
                plan.error = QStringLiteral("Cannot read migration input: %1").arg(relative);
                return plan;
            }
            QJsonParseError parseError;
            const auto json = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
                plan.ok = false;
                plan.error = QStringLiteral("Invalid migration input: %1").arg(relative);
                return plan;
            }
            documents.insert(relative, json.object());
        }
        LabelReferences labels(documents);
        for (auto& document : documents) {
            auto header = document.value(QStringLiteral("harkenDocument")).toObject();
            header = ProjectDocument::createHeader(header.value(QStringLiteral("type")).toString(),
                header.value(QStringLiteral("id")).toString(),
                header.value(QStringLiteral("projectId")).toString(), header);
            header.insert(QStringLiteral("schemaVersion"), step.toSchema);
            auto content = document.value(QStringLiteral("content")).toObject();
            if (header.value(QStringLiteral("type")) == QStringLiteral("project.root")
                && !content.contains(QStringLiteral("audio"))) {
                auto metadata = content.value(QStringLiteral("metadata")).toObject();
                content.insert(QStringLiteral("audio"), QJsonObject{
                    {QStringLiteral("renderSampleRate"), metadata.take(QStringLiteral("sampleRate"))},
                    {QStringLiteral("channelConfig"), metadata.take(QStringLiteral("channelConfig"))}});
                content.insert(QStringLiteral("deployPlatforms"), metadata.take(QStringLiteral("platforms")));
                content.insert(QStringLiteral("deployLanguages"), content.take(QStringLiteral("projectLanguages")));
                content.insert(QStringLiteral("metadata"), metadata);
            }
            if (!migrateLegacyContent(&content, labels, &plan.error)
                || !ProjectDocument::normalizeContent(&content, &plan.error)) {
                plan.ok = false;
                return plan;
            }
            document.insert(QStringLiteral("harkenDocument"), header);
            document.insert(QStringLiteral("content"), content);
        }
        labels.writeTo(documents);
        QStringList migrationWarnings;
        const QString relativeProject = QDir::fromNativeSeparators(QDir(projectDirectory).relativeFilePath(projectFile));
        if (!prepareConversionMigration(projectDirectory, relativeProject, documents,
                                         plan.inputFingerprints, &migrationWarnings, &plan.error)) {
            plan.ok = false;
            return plan;
        }
        step.writeSet = documents.keys();
        plan.writeSet.append(step.writeSet);
        plan.writeSet.sort();
        plan.writeSet.removeDuplicates();
        step.transform = [documents, migrationWarnings](ProjectMigrationWorkspace& workspace,
                                                        QStringList* warnings, QString* error) {
            for (auto entry = documents.cbegin(); entry != documents.cend(); ++entry)
                if (!workspace.writeObject(entry.key(), entry.value(), error)) return false;
            if (warnings) warnings->append(migrationWarnings);
            return true;
        };
    }
    plan.fileCount = plan.writeSet.size();
    plan.recoveryBytes = 0;
    for (const QString& path : plan.writeSet) {
        const QFileInfo info(QDir(projectDirectory).filePath(path));
        if (info.isFile())
            plan.recoveryBytes += info.size();
    }
    return plan;
}

bool MigrationManager::migrateProject(const ProjectMigrationPlan& plan,
                                      const QString& sourceDirectory,
                                      const QString& stagingDirectory,
                                      const QString& projectFile,
                                      QStringList* warnings,
                                      QString* error,
                                      ProjectMigrationProgressCallback progress) const
{
    const QString relative = ProjectDataPolicy::normalizeRelativePath(
        QDir(sourceDirectory).relativeFilePath(projectFile));
    ProjectMigrationWorkspace workspace(sourceDirectory, stagingDirectory,
                                        plan.writeSet, relative);
    const auto validator = [this](ProjectMigrationWorkspace& migrationWorkspace,
                              int expectedVersion,
                              QString* validationError) {
        QJsonObject root;
        if (!migrationWorkspace.readObject(migrationWorkspace.projectFile(), &root,
                                           validationError))
            return false;
        const int actualVersion = root.value(QStringLiteral("harkenDocument"))
                                      .toObject()
                                      .value(QStringLiteral("schemaVersion"))
                                      .toInt(0);
        if (actualVersion != expectedVersion) {
            if (validationError)
                *validationError = QStringLiteral("PROJECT_MIGRATION_VERSION_MISMATCH");
            return false;
        }
        if (treeValidatorForTesting_)
            return treeValidatorForTesting_(migrationWorkspace.stagingDirectory(),
                                            expectedVersion, validationError);
        const QStringList errors = ProjectContractRegistry::instance().validateSnapshot(
            migrationWorkspace.stagingDirectory(), root, false,
            migrationWorkspace.loadedObjects());
        if (errors.isEmpty())
            return true;
        if (validationError)
            *validationError = QStringLiteral("PROJECT_MIGRATION_VALIDATION_FAILED:")
                + errors.first();
        return false;
    };
    return registry_.execute(plan, workspace, warnings, error,
                             std::move(progress), validator);
}

}
