
#include "MigrationManager.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectDataPolicy.h"
#include "ProjectContract.h"
#include "ProjectTypes.h"
#include "harken_buildinfo.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <array>
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

constexpr std::array<qint64, 8> kSampleRates{8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};

void warnDiscarded(const QJsonObject& content, const char* field)
{
    qWarning() << "[MigrationManager] Discarded retired" << field << "from"
               << content.value(QStringLiteral("name")).toString()
               << content.value(QStringLiteral("guid")).toString();
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

void renameField(QJsonObject& object, const QString& from, const QString& to)
{
    if (object.contains(from)) object.insert(to, object.take(from));
}

QJsonObject migrateHeader(QJsonObject header, int targetSchema)
{
    header.insert(QStringLiteral("schemaVersion"), targetSchema);
    for (const auto* key : {"propertyModel", "savedStudioVersion", "packSchemaVersion"})
        header.remove(QLatin1String(key));
    if (header.value(QStringLiteral("type")) == QStringLiteral("project.root"))
        header.insert(QStringLiteral("studioVersion"), QString::fromUtf8(
            Harken::BuildInfo::Version.data(),
            static_cast<int>(Harken::BuildInfo::Version.size())));
    return header;
}

void migrateRoot(QJsonObject& content)
{
    auto metadata = content.value(QStringLiteral("metadata")).toObject();
    content.insert(QStringLiteral("audio"), QJsonObject{
        {QStringLiteral("renderSampleRate"), metadata.take(QStringLiteral("sampleRate"))},
        {QStringLiteral("channelConfig"), metadata.take(QStringLiteral("channelConfig"))}});
    content.insert(QStringLiteral("deployPlatforms"), metadata.take(QStringLiteral("platforms")));
    renameField(content, QStringLiteral("projectLanguages"), QStringLiteral("deployLanguages"));
    for (const auto* key : {"createdDate", "modifiedDate", "toolVersion", "version",
                            "projectTemplateId", "enableLFE", "author", "description"})
        metadata.remove(QLatin1String(key));
    content.insert(QStringLiteral("metadata"), metadata);
}

void migratePlayableProperties(QJsonObject& properties)
{
    constexpr uint64_t retiredBits = UINT64_C(0x4) | UINT64_C(0x1000) | UINT64_C(0x8000);
    for (const auto* key : {"makeUpGain", "spatial.attenuationCurveType", "signalGraph"})
        properties.remove(QLatin1String(key));
    const uint64_t bits = properties.value(QStringLiteral("presence")).toString().toULongLong(nullptr, 16);
    if ((bits & retiredBits) != 0)
        properties.insert(QStringLiteral("presence"),
            PlayablePropertyAuthoringV1::encodePresence(bits & ~retiredBits));
    renameField(properties, QStringLiteral("mainOutputBusRef"), QStringLiteral("outputBusRef"));
    editObjects(properties, QStringLiteral("platformOverrides"), migratePlayableProperties);
}

void migrateEffects(QJsonObject& mix)
{
    editObjects(mix, QStringLiteral("effects"), [](QJsonObject& effect) {
        if (!effect.contains(QStringLiteral("type"))) return;
        effect.insert(QStringLiteral("custom"), QJsonObject{
            {QStringLiteral("type"), effect.take(QStringLiteral("type"))},
            {QStringLiteral("parameters"), effect.take(QStringLiteral("parameters"))}});
        effect.insert(QStringLiteral("mode"), QStringLiteral("custom"));
    });
}

void migrateFieldGroups(QJsonObject& groups, QJsonObject& mix)
{
    const auto migrateOutput = [](QJsonObject& scope) {
        renameField(scope, QStringLiteral("mainOutput"), QStringLiteral("output"));
        editObject(scope, QStringLiteral("output"), [](QJsonObject& output) {
            editObject(output, QStringLiteral("values"), [](QJsonObject& values) {
                renameField(values, QStringLiteral("mainOutputBusRef"), QStringLiteral("outputBusRef"));
            });
        });
    };
    editObject(groups, QStringLiteral("shared"), [&](QJsonObject& shared) {
        migrateOutput(shared);
        shared.insert(QStringLiteral("effect"), QJsonObject{
            {QStringLiteral("inherit"), false},
            {QStringLiteral("values"), QJsonObject{
                {QStringLiteral("effects"), mix.take(QStringLiteral("effects")).toArray()}}}});
    });
    editObject(groups, QStringLiteral("platforms"), [&](QJsonObject& platforms) {
        for (const auto& id : platforms.keys()) editObject(platforms, id, migrateOutput);
    });
}

bool isDefaultTransitionTable(const QJsonObject& table)
{
    const auto isDefaultFade = [](const QJsonValue& value) {
        const auto fade = value.toObject();
        return fade.value(QStringLiteral("durationSeconds")).toDouble() == 0.5
            && fade.value(QStringLiteral("curve")).toArray().size() == 2;
    };
    const auto rules = table.value(QStringLiteral("rules")).toArray();
    if (rules.size() != 1) return false;
    const auto rule = rules.first().toObject();
    return rule.value(QStringLiteral("source")).toString().isEmpty()
        && rule.value(QStringLiteral("target")).toString().isEmpty()
        && rule.value(QStringLiteral("bridge")).toString().isEmpty()
        && rule.value(QStringLiteral("exitTiming")).toString() == QStringLiteral("Immediate")
        && rule.value(QStringLiteral("entryMode")).toString() == QStringLiteral("EntryMarker")
        && isDefaultFade(rule.value(QStringLiteral("sourceFadeOut")))
        && isDefaultFade(rule.value(QStringLiteral("targetFadeIn")));
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
    static const QUuid nameSpace(QStringLiteral("ff37991a-e8d0-4d2b-99c3-3f9ad6a4c33a"));
    QMap<QUuid, QStringList> rowsByVariable;
    auto rows = table.value(QStringLiteral("blendAxes")).toArray();
    for (qsizetype index = 0; index < rows.size(); ++index) {
        if (!rows[index].isObject()) {
            const QUuid variable(rows[index].toString());
            rows[index] = QJsonObject{
                {QStringLiteral("guid"), QUuid::createUuidV5(nameSpace, variable.toRfc4122())
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
    QVector<QPair<qint64, QJsonObject>> rows;
    editObjects(table, QStringLiteral("entries"), [&](QJsonObject& entry) {
        const auto target = entry.value(QStringLiteral("childGuid"));
        for (const auto& value : entry.take(QStringLiteral("conditions")).toArray()) {
            auto row = value.toObject();
            const qint64 priority = row.take(QStringLiteral("priority")).toInteger();
            row.insert(QStringLiteral("target"), target);
            rows.append({priority, row});
        }
    });
    std::stable_sort(rows.begin(), rows.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    const auto axes = table.value(QStringLiteral("matchAxes")).toArray();
    QJsonArray matchRows;
    for (auto& entry : rows) {
        auto& row = entry.second;
        auto cells = row.value(QStringLiteral("cells")).toArray();
        for (qsizetype axis = 0; axis < cells.size(); ++axis) {
            if (!cells[axis].isDouble()) continue;
            const auto label = labels.resolve(QUuid(axes.at(axis).toString()), cells[axis].toInt());
            if (label.isEmpty()) {
                if (error)
                    *error = QStringLiteral("Container '%1' Match rule %2 column %3 value %4 has no unique Label")
                        .arg(content.value(QStringLiteral("name")).toString())
                        .arg(matchRows.size() + 1).arg(axis + 1).arg(cells[axis].toInt());
                return false;
            }
            cells[axis] = QJsonObject{{QStringLiteral("labelId"), label}};
        }
        row.insert(QStringLiteral("cells"), cells);
        matchRows.append(row);
    }
    table.insert(QStringLiteral("matchRows"), matchRows);
    return true;
}

bool migrateSelection(QJsonObject& selection, const QJsonObject& content,
                      LabelReferences& labels, QString* error)
{
    if (selection.contains(QStringLiteral("transitions"))
        && !isDefaultTransitionTable(selection.take(QStringLiteral("transitions")).toObject()))
        warnDiscarded(content, "transitions");
    if (selection.take(QStringLiteral("spliceByMarker")).toBool())
        warnDiscarded(content, "spliceByMarker");
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

void migrateSegment(QJsonObject& components, const QJsonObject& content)
{
    renameField(components, QStringLiteral("piecetimeline"), QStringLiteral("segmenttimeline"));
    editObject(components, QStringLiteral("segmenttimeline"), [&](QJsonObject& timeline) {
        const auto time = timeline.take(QStringLiteral("pieceTime")).toObject();
        if (time.value(QStringLiteral("domain")).toString() == QStringLiteral("Musical"))
            warnDiscarded(content, "musical time");
        if (!timeline.take(QStringLiteral("markers")).toArray().isEmpty())
            warnDiscarded(content, "markers");
    });
}

void discardActionTiming(QJsonObject& owner, const QString& key, const QJsonObject& content)
{
    editObjects(owner, key, [&](QJsonObject& step) {
        if (!step.contains(QStringLiteral("quantize"))) return;
        step.remove(QStringLiteral("quantize"));
        warnDiscarded(content, "Action quantize");
    });
}

bool migrateContent(QJsonObject& content, LabelReferences& labels, QString* error)
{
    for (const auto* key : {"expanded", "objectPath", "canvasPos", "nameBindingRevision",
                            "actionPublishedNameHistory", "paths", "path", "sourceID",
                            "deploymentProfiles", "extensions", "packExportCurrentPlatformId",
                            "packExportSelectedPlatformIds", "actionPackTargeting", "packOutputDir",
                            "unityIntegration", "conversionFailurePolicy", "packGroups"})
        content.remove(QLatin1String(key));
    if (content.value(QStringLiteral("type")) == QStringLiteral("Piece"))
        content.insert(QStringLiteral("type"), QStringLiteral("Segment"));
    auto components = content.value(QStringLiteral("components")).toObject();
    if (content.contains(QStringLiteral("componentData")))
        components.insert(QStringLiteral("bus"), content.take(QStringLiteral("componentData")));
    auto mix = components.value(QStringLiteral("mixnode")).toObject();
    mix.remove(QStringLiteral("makeUpGain"));
    migrateEffects(mix);
    editObject(content, QStringLiteral("fieldGroups"),
               [&](QJsonObject& groups) { migrateFieldGroups(groups, mix); });
    if (components.contains(QStringLiteral("mixnode")))
        components.insert(QStringLiteral("mixnode"), mix);
    editObject(content, QStringLiteral("playableProperties"), migratePlayableProperties);
    discardActionTiming(content, QStringLiteral("actions"), content);
    editObject(components, QStringLiteral("action"), [&](QJsonObject& action) {
        discardActionTiming(action, QStringLiteral("steps"), content);
    });
    editObject(components, QStringLiteral("tracktimeline"), [](QJsonObject& timeline) {
        editObjects(timeline, QStringLiteral("clips"), [](QJsonObject& clip) {
            editObject(clip, QStringLiteral("playableProperties"), migratePlayableProperties);
        });
    });
    migrateSegment(components, content);
    bool valid = true;
    editObject(components, QStringLiteral("selection"), [&](QJsonObject& selection) {
        valid = migrateSelection(selection, content, labels, error);
    });
    if (!components.isEmpty()) content.insert(QStringLiteral("components"), components);
    editObjects(content, QStringLiteral("children"), [&](QJsonObject& child) {
        valid = valid && migrateContent(child, labels, error);
    });
    return valid;
}

QJsonObject conversionGroup(const QString& id)
{
    return {{QStringLiteral("inherit"), false},
            {QStringLiteral("values"), QJsonObject{{QStringLiteral("settingsRef"),
                id.isEmpty() ? QUuid().toString(QUuid::WithoutBraces) : id}}}};
}

qint64 conversionRate(qint64 rate)
{
    if (rate > kSampleRates.back()) return 0;
    qint64 selected = kSampleRates.front();
    for (const auto allowed : kSampleRates)
        if (allowed <= rate) selected = allowed;
    return selected;
}

QJsonArray conversionRules(const QJsonObject& profile)
{
    QJsonArray rules;
    if (!profile.value(QStringLiteral("performResample")).toBool()) return rules;
    for (const auto& value : profile.value(QStringLiteral("sizePolicies")).toArray()) {
        const auto policy = value.toObject();
        rules.append(QJsonObject{
            {QStringLiteral("maxBytes"), policy.value(QStringLiteral("maxBytes"))},
            {QStringLiteral("sampleRate"), conversionRate(policy.value(QStringLiteral("config")).toObject()
                .value(QStringLiteral("targetSampleRate")).toInteger())},
            {QStringLiteral("format"), QStringLiteral("PCM")}});
    }
    return rules;
}

void migrateConversions(const QString& projectFile, int targetSchema,
                        QMap<QString, QJsonObject>& documents)
{
    auto rootDocument = documents.value(projectFile);
    auto root = rootDocument.value(QStringLiteral("content")).toObject();
    const auto projectId = rootDocument.value(QStringLiteral("harkenDocument")).toObject()
        .value(QStringLiteral("projectId")).toString();
    QMap<QByteArray, QString> presets;
    QMap<QString, QJsonObject> created;
    QMap<QString, QString> presetByPlatform;
    QSet<QString> names;
    const auto addPreset = [&](const QJsonArray& rules) {
        const auto key = QJsonDocument(rules).toJson(QJsonDocument::Compact);
        if (presets.contains(key)) return presets.value(key);
        const QString name = NameValidator::makeUniqueName(QStringLiteral("Conversion"),
            [&](const QString& candidate) { return !names.contains(candidate.toCaseFolded()); });
        names.insert(name.toCaseFolded());
        const QUuid guid = QUuid::createUuidV5(QUuid(projectId),
            QByteArrayLiteral("conversion-migration/") + key);
        const QString id = guid.toString(QUuid::WithoutBraces);
        created.insert(QStringLiteral("Conversions/") + NameValidator::objectFileName(name, guid), {
            {QStringLiteral("harkenDocument"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("project.conversion")},
                {QStringLiteral("id"), id}, {QStringLiteral("projectId"), projectId},
                {QStringLiteral("schemaVersion"), targetSchema}}},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("Conversion")},
                {QStringLiteral("guid"), id}, {QStringLiteral("name"), name},
                {QStringLiteral("parentRef"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}}},
                {QStringLiteral("components"), QJsonObject{{QStringLiteral("conversion"),
                    QJsonObject{{QStringLiteral("rules"), rules}}}}}}}});
        presets.insert(key, id);
        return id;
    };
    QJsonObject defaultConversions;
    editObjects(root, QStringLiteral("deployPlatforms"), [&](QJsonObject& platform) {
        const auto rules = conversionRules(platform.take(QStringLiteral("profile")).toObject());
        if (rules.isEmpty()) return;
        const auto platformId = platform.value(QStringLiteral("id")).toString();
        const auto id = addPreset(rules);
        presetByPlatform.insert(platformId, id);
        defaultConversions.insert(platformId, QJsonObject{
            {QStringLiteral("id"), id}, {QStringLiteral("documentId"), id}});
    });
    if (!defaultConversions.isEmpty())
        editObject(root, QStringLiteral("audio"), [&](QJsonObject& audio) {
            audio.insert(QStringLiteral("defaultConversions"), defaultConversions);
        });
    rootDocument.insert(QStringLiteral("content"), root);
    documents.insert(projectFile, rootDocument);

    const std::function<void(QJsonObject&, bool)> migrateNode = [&](QJsonObject& node, bool topLevel) {
        editObject(node, QStringLiteral("fieldGroups"), [&](QJsonObject& groups) {
            editObject(groups, QStringLiteral("shared"), [&](QJsonObject& shared) {
                shared.insert(QStringLiteral("conversion"), topLevel ? conversionGroup({})
                    : QJsonObject{{QStringLiteral("inherit"), true}});
            });
            if (!topLevel || presetByPlatform.isEmpty()) return;
            auto platforms = groups.value(QStringLiteral("platforms")).toObject();
            for (auto preset = presetByPlatform.cbegin(); preset != presetByPlatform.cend(); ++preset) {
                auto scope = platforms.value(preset.key()).toObject();
                scope.insert(QStringLiteral("conversion"), conversionGroup(preset.value()));
                platforms.insert(preset.key(), scope);
            }
            groups.insert(QStringLiteral("platforms"), platforms);
        });
        editObjects(node, QStringLiteral("children"),
                    [&](QJsonObject& child) { migrateNode(child, false); });
    };
    for (auto& document : documents)
        editObject(document, QStringLiteral("content"), [&](QJsonObject& content) {
            migrateNode(content, content.value(QStringLiteral("parentRef")).toObject()
                .value(QStringLiteral("kind")) == QStringLiteral("root"));
        });
    documents.insert(created);
}

bool migrateDocuments(const QString& projectFile, int targetSchema,
                      QMap<QString, QJsonObject>& documents, QString* error)
{
    LabelReferences labels(documents);
    for (auto document = documents.begin(); document != documents.end(); ++document) {
        document->insert(QStringLiteral("harkenDocument"),
            migrateHeader(document->value(QStringLiteral("harkenDocument")).toObject(), targetSchema));
        auto content = document->value(QStringLiteral("content")).toObject();
        if (document.key() == projectFile) migrateRoot(content);
        if (!migrateContent(content, labels, error)) return false;
        document->insert(QStringLiteral("content"), content);
    }
    labels.writeTo(documents);
    migrateConversions(projectFile, targetSchema, documents);
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
        const QString relativeProject = QDir::fromNativeSeparators(QDir(projectDirectory).relativeFilePath(projectFile));
        if (!migrateDocuments(relativeProject, step.toSchema, documents, &plan.error)) {
            plan.ok = false;
            return plan;
        }
        step.writeSet = documents.keys();
        plan.writeSet.append(step.writeSet);
        plan.writeSet.sort();
        plan.writeSet.removeDuplicates();
        step.transform = [documents](ProjectMigrationWorkspace& workspace,
                                     QStringList*, QString* error) {
            for (auto entry = documents.cbegin(); entry != documents.cend(); ++entry)
                if (!workspace.writeObject(entry.key(), entry.value(), error)) return false;
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
