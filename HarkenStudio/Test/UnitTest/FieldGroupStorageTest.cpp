#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSpatial.h"
#include "Core/ProjectContract.h"
#include "Core/ProjectDocument.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSchemaGenerated.h"
#include "Core/ProjectSchemaValidator.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTransaction.h"
#include "Core/FieldGroupJson.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace VNX {
namespace {

QUuid guid(std::uint64_t value)
{
    return QUuid(QStringLiteral("00000000-0000-0000-0000-%1")
        .arg(value, 12, 16, QLatin1Char('0')));
}

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, std::uint32_t id)
{
    auto item = std::make_shared<ProjectItem>(QStringLiteral("Item%1").arg(id), type);
    item->guid = guid(id);
    item->objectID = id;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(id));
    item->attachDefaultComponents();
    return item;
}

DefinitionFieldGroups rememberedGroups()
{
    DefinitionFieldGroups groups;
    const harkenprojectschema::AuthoringGuid missing{UINT64_C(0x123456789abcdef0), UINT64_MAX};
    groups.mainOutput.shared = {FieldGroupMode::Inherit, MainOutputFieldGroup{missing, -6.0f}};
    groups.send.shared = {FieldGroupMode::Inherit, SendFieldGroup{{{missing, -12.0f}}}};
    SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = missing;
    rating.spatialAttenuationDistanceScaling = 2.7f;
    groups.spatialRating.shared = {FieldGroupMode::Inherit, rating};
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::UseShared, MainOutputFieldGroup{missing, -3.0f}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, SendFieldGroup{}};
    return groups;
}

QJsonObject groupJson(const DefinitionFieldGroups& groups = {})
{
    std::string text;
    std::string error;
    EXPECT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
}

QJsonObject soundJson()
{
    auto item = makeItem(ProjectItem::Type::Sound, 1);
    ProjectSerializer serializer;
    return serializer.projectItemToJson(*item);
}

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

QJsonObject withMissingMarker(QJsonObject document)
{
    auto header = document.value(QStringLiteral("harkenDocument")).toObject();
    header.remove(QStringLiteral("propertyModel"));
    document[QStringLiteral("harkenDocument")] = header;
    return document;
}

TEST(FieldGroupStorage, EveryDocumentHeaderRequiresTheGeneratedPropertyModel)
{
    const auto id = guid(1).toString(QUuid::WithoutBraces);
    for (const auto* kind : {"project.root", "project.layer", "project.bus", "project.action",
                             "project.attenuation", "project.variable"}) {
        SCOPED_TRACE(kind);
        const QString type = QString::fromLatin1(kind);
        const auto document = ProjectDocument::create(type, id, id, {});
        const auto header = document.value(QStringLiteral("harkenDocument")).toObject();
        EXPECT_EQ(header.value(QStringLiteral("propertyModel")).toString(),
                  QString::fromLatin1(ProjectSchema::kProjectPropertyModel));
        ProjectDocumentData parsed;
        QString error;
        ASSERT_TRUE(ProjectDocument::parse(document, type, &parsed, &error)) << error.toStdString();
        parsed.id = QStringLiteral("unchanged");
        EXPECT_FALSE(ProjectDocument::parse(withMissingMarker(document), type, &parsed, &error));
        EXPECT_EQ(parsed.id, QStringLiteral("unchanged"));
        auto bad = document;
        auto badHeader = header;
        badHeader[QStringLiteral("propertyModel")] = true;
        bad[QStringLiteral("harkenDocument")] = badHeader;
        EXPECT_FALSE(ProjectDocument::parse(bad, type, &parsed, &error));
        EXPECT_EQ(parsed.id, QStringLiteral("unchanged"));
    }
}

TEST(FieldGroupStorage, FiveDefinitionKindsRoundTripRememberedGroupsThroughBothEntrypoints)
{
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container,
                            ProjectItem::Type::Layer, ProjectItem::Type::Track, ProjectItem::Type::Piece}) {
        SCOPED_TRACE(static_cast<int>(type));
        const auto item = makeItem(type, 1);
        item->fieldGroups = rememberedGroups();
        for (const bool standalone : {false, true}) {
            ProjectSerializer writer;
            const auto json = standalone ? writer.audioBranchToJson(*item) : writer.projectItemToJson(*item);
            ASSERT_FALSE(json.isEmpty()) << writer.lastError().toStdString();
            ASSERT_EQ(json.value(QStringLiteral("fieldGroups")).toObject(), groupJson(item->fieldGroups));
            ProjectSerializer reader;
            ProjectItem loaded(QStringLiteral("Destination"), ProjectItem::Type::None);
            const bool ok = standalone ? reader.audioBranchFromJson(json, loaded)
                                       : reader.projectItemFromJson(json, loaded);
            ASSERT_TRUE(ok) << reader.lastError().toStdString();
            EXPECT_EQ(loaded.fieldGroups, item->fieldGroups);
            EXPECT_FALSE(loaded.fieldGroups.send.platforms.at(HarkenTest::FieldGroupPlatformIds::Android)
                .localHistory->sends.size());
            EXPECT_EQ(loaded.fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).mode,
                      FieldGroupMode::UseShared);
        }
    }
}

TEST(FieldGroupStorage, ComponentCachesCannotBecomeDefinitionGroupAuthority)
{
    auto item = makeItem(ProjectItem::Type::Sound, 1);
    item->comp<VMixNode>()->outputBusId = 999;
    item->comp<VMixNode>()->mainOutputBusRef = harkentest::StudioObjectId(999);
    item->comp<VMixNode>()->outputBusVolume = -20.0f;
    item->comp<VMixNode>()->sends.push_back({999, 0.5f});
    item->comp<VAudio>()->overrideBits = 1;
    auto* spatial = item->comp<VSpatial>();
    ASSERT_NE(spatial, nullptr);
    spatial->is3DEnabled = true;
    spatial->spatializationMix = 0.5f;
    ProjectSerializer serializer;
    const auto json = serializer.projectItemToJson(*item);
    ASSERT_FALSE(json.isEmpty()) << serializer.lastError().toStdString();
    EXPECT_EQ(json.value(QStringLiteral("fieldGroups")).toObject(), groupJson());
    EXPECT_FALSE(json.contains(QStringLiteral("outputBusOverride")));
    const auto components = json.value(QStringLiteral("components")).toObject();
    const auto mix = components.value(QStringLiteral("mixnode")).toObject();
    for (const auto* key : {"outputBusId", "outputBusRef", "outputBusVolume", "mainOutputBusRef", "sends"})
        EXPECT_FALSE(mix.contains(QString::fromLatin1(key)));
    const auto audio = components.value(QStringLiteral("audio")).toObject();
    EXPECT_FALSE(audio.contains(QStringLiteral("outputBusOverride")));
    EXPECT_FALSE(audio.contains(QStringLiteral("overrideBits")));
    const auto spatialJson = components.value(QStringLiteral("spatial")).toObject();
    EXPECT_DOUBLE_EQ(spatialJson.value(QStringLiteral("spatializationMix")).toDouble(), 0.5);
    EXPECT_FALSE(spatialJson.contains(QStringLiteral("is3DEnabled")));
    EXPECT_FALSE(spatialJson.contains(QStringLiteral("spread")));
    EXPECT_FALSE(spatialJson.contains(QStringLiteral("focus")));
    ProjectItem loaded(QStringLiteral("Destination"), ProjectItem::Type::None);
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded)) << serializer.lastError().toStdString();
    EXPECT_EQ(loaded.fieldGroups, DefinitionFieldGroups{});
}

TEST(FieldGroupStorage, ReaderRejectsLegacyDefinitionChannelsBeforeChangingTheDestination)
{
    const std::array<std::function<void(QJsonObject&)>, 8> changes{
        [](auto& json) { json.remove(QStringLiteral("fieldGroups")); },
        [](auto& json) { json[QStringLiteral("outputBusOverride")] = false; },
        [](auto& json) { json[QStringLiteral("playableProperties")] = QJsonObject{
            {QStringLiteral("presence"), QStringLiteral("0000000000000020")}}; },
        [](auto& json) { auto properties = json.value(QStringLiteral("playableProperties")).toObject();
            properties[QStringLiteral("sends")] = QJsonArray{};
            json[QStringLiteral("playableProperties")] = properties; },
        [](auto& json) { json[QStringLiteral("components")] = QJsonObject{
            {QStringLiteral("mixnode"), QJsonObject{{QStringLiteral("outputBusId"), 999}}}}; },
        [](auto& json) { json[QStringLiteral("components")] = QJsonObject{
            {QStringLiteral("spatial"), QJsonObject{{QStringLiteral("is3DEnabled"), false}}}}; },
        [](auto& json) { json[QStringLiteral("components")] = QJsonObject{
            {QStringLiteral("audio"), QJsonObject{{QStringLiteral("overrideBits"), 0}}}}; },
        [](auto& json) { auto properties = json.value(QStringLiteral("playableProperties")).toObject();
            properties[QStringLiteral("platformOverrides")] = QJsonArray{QJsonObject{
                {QStringLiteral("platform"), QStringLiteral("Windows")},
                {QStringLiteral("presence"), QStringLiteral("0000000000002000")}}};
            json[QStringLiteral("playableProperties")] = properties; }
    };
    for (const auto& change : changes) {
        auto json = soundJson();
        ASSERT_FALSE(json.isEmpty());
        change(json);
        ProjectItem destination(QStringLiteral("Unchanged"), ProjectItem::Type::Sound);
        destination.guid = guid(777);
        destination.fieldGroups = rememberedGroups();
        ProjectSerializer serializer;
        EXPECT_FALSE(serializer.projectItemFromJson(json, destination));
        EXPECT_EQ(destination.name, QStringLiteral("Unchanged"));
        EXPECT_EQ(destination.guid, guid(777));
        EXPECT_EQ(destination.fieldGroups, rememberedGroups());
        EXPECT_FALSE(serializer.audioBranchFromJson(json, destination));
        EXPECT_EQ(destination.name, QStringLiteral("Unchanged"));
    }
}

TEST(FieldGroupStorage, MissingLocalPayloadIsRejectedByTheGeneratedSchemaAndCodec)
{
    auto json = soundJson();
    auto groups = json.value(QStringLiteral("fieldGroups")).toObject();
    auto shared = groups.value(QStringLiteral("shared")).toObject();
    shared[QStringLiteral("mainOutput")] = QJsonObject{{QStringLiteral("inherit"), false}};
    groups[QStringLiteral("shared")] = shared;
    json[QStringLiteral("fieldGroups")] = groups;
    QString error;
    EXPECT_FALSE(ProjectDocument::validateFieldGroupAuthoring(json, &error));
    const auto id = guid(1).toString(QUuid::WithoutBraces);
    QJsonObject document{
        {QStringLiteral("harkenDocument"), ProjectDocument::createHeader(QStringLiteral("project.layer"), id, id)},
        {QStringLiteral("content"), json}};
    const auto errors = ProjectSchemaValidator::validateProjectDocument(
        ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"), document,
        QStringLiteral("Layers/Test.vp"));
    EXPECT_TRUE(errors.join(QChar('\n')).contains(QStringLiteral("values")));
    EXPECT_TRUE(ProjectDocument::create(QStringLiteral("project.layer"), id, id, json).isEmpty());
}

TEST(FieldGroupStorage, StandalonePrecheckRejectsAChildBeforeMintingAnyIdentity)
{
    auto root = makeItem(ProjectItem::Type::Layer, 2);
    ProjectSerializer writer;
    auto json = writer.projectItemToJson(*root);
    auto child = soundJson();
    child.remove(QStringLiteral("fieldGroups"));
    json[QStringLiteral("children")] = QJsonArray{child};
    ProjectSerializer reader;
    EXPECT_FALSE(reader.prepareStandaloneReferenceRead(QJsonArray{json}));
    EXPECT_EQ(reader.transientObjectIdForGuid(root->guid), 0u);
    EXPECT_EQ(reader.transientObjectIdForGuid(guid(1)), 0u);
}

TEST(FieldGroupStorage, BusKeepsItsLocalPropertyPayloadAndCannotAuthorGroups)
{
    auto bus = makeItem(ProjectItem::Type::Bus, 100);
    bus->playablePropertyPresence.bits = UINT64_C(0x20);
    bus->comp<VMixNode>()->outputBusVolume = -6.0f;
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    project.busses.push_back({bus, {}});
    ProjectSerializer writer;
    ASSERT_TRUE(writer.prepareReferenceWrite(project));
    auto json = writer.busPackToJson(*bus);
    ASSERT_FALSE(json.isEmpty()) << writer.lastError().toStdString();
    EXPECT_FALSE(json.contains(QStringLiteral("fieldGroups")));
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("playableProperties")).toObject()
        .value(QStringLiteral("outputBusVolume")).toDouble(), -6.0);
    QString error;
    EXPECT_TRUE(ProjectDocument::validateFieldGroupAuthoring(json, &error));
    json[QStringLiteral("fieldGroups")] = groupJson();
    EXPECT_FALSE(ProjectDocument::validateFieldGroupAuthoring(json, &error));
    bus->fieldGroups.send.shared = {FieldGroupMode::Local, SendFieldGroup{}};
    EXPECT_TRUE(writer.busPackToJson(*bus).isEmpty());
}

TEST(FieldGroupStorage, NonDefinitionPackWritersCannotDiscardAuthoredGroups)
{
    ProjectSerializer serializer;
    for (const auto& [type, write] : std::array{
             std::pair{ProjectItem::Type::Action, &ProjectSerializer::actionPackToJson},
             std::pair{ProjectItem::Type::Attenuation, &ProjectSerializer::attenuationPackToJson},
             std::pair{ProjectItem::Type::Variable, &ProjectSerializer::variablePackToJson}}) {
        auto item = makeItem(type, 1);
        ASSERT_FALSE((serializer.*write)(*item).isEmpty());
        item->fieldGroups = rememberedGroups();
        EXPECT_TRUE((serializer.*write)(*item).isEmpty());
        EXPECT_TRUE(serializer.projectItemToJson(*item).isEmpty());
    }
}

TEST(FieldGroupStorage, OccurrenceInputsRejectGroupsWhileMixingRemainsAllowed)
{
    auto track = makeItem(ProjectItem::Type::Track, 1);
    ProjectSerializer serializer;
    auto json = serializer.projectItemToJson(*track);
    auto components = json.value(QStringLiteral("components")).toObject();
    auto timeline = components.value(QStringLiteral("tracktimeline")).toObject();
    QJsonObject clip{{QStringLiteral("playableProperties"), QJsonObject{
        {QStringLiteral("presence"), QStringLiteral("0000000000000003")},
        {QStringLiteral("volume"), -6.0}, {QStringLiteral("pitch"), 3.0}}}};
    const auto withClip = [&](const QJsonObject& value) {
        auto result = json;
        auto timelineValue = timeline;
        timelineValue[QStringLiteral("clips")] = QJsonArray{value};
        auto componentValue = components;
        componentValue[QStringLiteral("tracktimeline")] = timelineValue;
        result[QStringLiteral("components")] = componentValue;
        return result;
    };
    QString error;
    EXPECT_TRUE(ProjectDocument::validateFieldGroupAuthoring(withClip(clip), &error));
    clip[QStringLiteral("fieldGroups")] = groupJson();
    EXPECT_FALSE(ProjectDocument::validateFieldGroupAuthoring(withClip(clip), &error));
    clip.remove(QStringLiteral("fieldGroups"));
    auto properties = clip.value(QStringLiteral("playableProperties")).toObject();
    properties[QStringLiteral("spatial.panning")] = 0;
    clip[QStringLiteral("playableProperties")] = properties;
    EXPECT_FALSE(ProjectDocument::validateFieldGroupAuthoring(withClip(clip), &error));
}

TEST(FieldGroupStorage, OldEmptyProjectIsRejectedBeforeTransactionRecoveryOrDestinationChanges)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("Project"));
    ASSERT_TRUE(QDir().mkpath(directory));
    const QString path = QDir(directory).filePath(QStringLiteral("Project.hsp"));
    const QString id = guid(1).toString(QUuid::WithoutBraces);
    const auto old = withMissingMarker(ProjectDocument::create(QStringLiteral("project.root"), id, id, {}));
    const QByteArray bytes = QJsonDocument(old).toJson();
    writeBytes(path, bytes);
    const QString journal = directory + QStringLiteral(".harken-project-transaction.json");
    writeBytes(journal, "{}");
    ASSERT_TRUE(ProjectTransaction::recoveryRequired(path));
    ProjectData destination;
    destination.name = QStringLiteral("Unchanged");
    const auto originalId = destination.projectId;
    ProjectSerializer serializer;
    EXPECT_FALSE(serializer.loadProject(path, destination, true));
    EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("property model")));
    EXPECT_EQ(destination.name, QStringLiteral("Unchanged"));
    EXPECT_EQ(destination.projectId, originalId);
    EXPECT_EQ(readBytes(path), bytes);
    EXPECT_EQ(readBytes(journal), QByteArray("{}"));
}

TEST(FieldGroupStorage, MixedProjectProbeRejectsOldSidecarsInEveryManagedDomain)
{
    for (const auto& [folder, type] : {
             std::pair{"Layers", "project.layer"}, std::pair{"Busses", "project.bus"},
             std::pair{"Actions", "project.action"}, std::pair{"Attenuations", "project.attenuation"},
             std::pair{"Variables", "project.variable"}}) {
        SCOPED_TRACE(folder);
        QTemporaryDir temporary;
        ASSERT_TRUE(temporary.isValid());
        const QString id = guid(1).toString(QUuid::WithoutBraces);
        const QString path = temporary.filePath(QStringLiteral("Project.hsp"));
        auto fixture = HarkenTest::projectWithPlatforms();
        fixture.projectId = id;
        fixture.busses.push_back({makeItem(ProjectItem::Type::Bus, 100), {}});
        ProjectSerializer writer;
        ASSERT_TRUE(writer.saveProject(path, fixture)) << writer.lastError().toStdString();
        ASSERT_TRUE(QDir(temporary.path()).mkpath(QString::fromLatin1(folder)));
        const QString packPath = temporary.filePath(QString::fromLatin1(folder) + QStringLiteral("/Old.vp"));
        const auto oldPack = withMissingMarker(ProjectDocument::create(QString::fromLatin1(type), id, id, {}));
        const QByteArray bytes = QJsonDocument(oldPack).toJson();
        writeBytes(packPath, bytes);
        const auto probe = ProjectContractRegistry::instance().probe(path);
        EXPECT_FALSE(probe.ok);
        EXPECT_TRUE(probe.detail.contains(QString::fromLatin1(folder)));
        EXPECT_TRUE(probe.detail.contains(QStringLiteral("property model")));
        bool approved = false;
        ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        ProjectSerializer serializer;
        EXPECT_FALSE(serializer.loadProject(path, project, true, {},
            [&](const auto&) { approved = true; return true; }));
        EXPECT_FALSE(approved);
        EXPECT_EQ(readBytes(packPath), bytes);
    }
}

TEST(FieldGroupStorage, SaveRejectsAnActiveMissingReferenceAndPreservesDormantHistory)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("Project.hsp"));
    writeBytes(path, "unchanged");
    auto master = makeItem(ProjectItem::Type::Bus, 100);
    auto layer = makeItem(ProjectItem::Type::Layer, 2);
    auto sound = makeItem(ProjectItem::Type::Sound, 1);
    layer->fieldGroups = MakeRootFieldGroups(master->runtimeId());
    layer->children.push_back(sound);
    sound->parent = layer.get();
    sound->fieldGroups = rememberedGroups();
    sound->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Local;
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    project.busses.push_back({master, {}});
    project.layers.push_back({layer, {}});
    ProjectSerializer serializer;
    EXPECT_FALSE(serializer.saveProject(path, project));
    EXPECT_EQ(readBytes(path), QByteArray("unchanged"));
    sound->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Inherit;
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    const auto document = QJsonDocument::fromJson(readBytes(path)).object();
    EXPECT_EQ(document.value(QStringLiteral("harkenDocument")).toObject()
        .value(QStringLiteral("propertyModel")).toString(), QStringLiteral("fieldGroupsV1"));
    EXPECT_EQ(sound->fieldGroups, rememberedGroups());
}

TEST(FieldGroupStorage, SaveRejectsMissingRootCurrentValuesWithoutChangingStoredBytes)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("Project.hsp"));
    auto master = makeItem(ProjectItem::Type::Bus, 100);
    auto sound = makeItem(ProjectItem::Type::Sound, 1);
    sound->fieldGroups = MakeRootFieldGroups(master->runtimeId());
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    project.busses.push_back({master, {}});
    project.layers.push_back({sound, {}});
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    const auto bytes = readBytes(path);
    const auto authored = sound->fieldGroups;
    for (const auto scope : {FieldGroupScope{}, FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Windows}}) {
        for (const auto group : {FieldGroupId::MainOutput, FieldGroupId::Send,
                                 FieldGroupId::SpatialRating}) {
            sound->fieldGroups = authored;
            const auto inherit = [scope](auto& value) {
                if (scope) value.platforms[*scope].mode = FieldGroupMode::Inherit;
                value.shared.localHistory.reset();
            };
            switch (group) {
            case FieldGroupId::MainOutput: inherit(sound->fieldGroups.mainOutput); break;
            case FieldGroupId::Send: inherit(sound->fieldGroups.send); break;
            case FieldGroupId::SpatialRating: inherit(sound->fieldGroups.spatialRating); break;
            }
            const auto invalid = sound->fieldGroups;
            EXPECT_FALSE(serializer.saveProject(path, project));
            EXPECT_EQ(readBytes(path), bytes);
            EXPECT_EQ(sound->fieldGroups, invalid);
        }
    }
}

TEST(FieldGroupStorage, RetiredPropertyModelIsRejectedWithoutChangingTheDocument)
{
    const auto id = guid(1).toString(QUuid::WithoutBraces);
    const auto current = ProjectDocument::create(QStringLiteral("project.layer"), id, id, soundJson());
    for (const bool conflict : {false, true}) {
        auto document = current;
        auto header = document.value(QStringLiteral("harkenDocument")).toObject();
        header.insert(QStringLiteral("propertyModel"), QStringLiteral("sectionGroupsV3"));
        document.insert(QStringLiteral("harkenDocument"), header);
        auto content = document.value(QStringLiteral("content")).toObject();
        content.insert(QStringLiteral("sectionGroups"), conflict
            ? content.value(QStringLiteral("fieldGroups")) : QJsonValue(true));
        if (!conflict) content.remove(QStringLiteral("fieldGroups"));
        document.insert(QStringLiteral("content"), content);
        const auto before = document;
        QString error;
        ProjectDocumentData parsed;
        parsed.id = QStringLiteral("unchanged");
        EXPECT_FALSE(ProjectDocument::parse(document, QStringLiteral("project.layer"), &parsed, &error));
        EXPECT_EQ(parsed.id, QStringLiteral("unchanged"));
        EXPECT_EQ(document, before);
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(FieldGroupStorage, PreviousGroupModelIsRejectedBeforeLoadingOrChangingTheProject)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("Project.hsp"));
    auto master = makeItem(ProjectItem::Type::Bus, 100);
    auto sound = makeItem(ProjectItem::Type::Sound, 1);
    sound->fieldGroups = MakeRootFieldGroups(master->runtimeId());
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    project.busses.push_back({master, {}});
    project.layers.push_back({sound, {}});
    ProjectSerializer writer;
    ASSERT_TRUE(writer.saveProject(path, project)) << writer.lastError().toStdString();
    auto document = QJsonDocument::fromJson(readBytes(path)).object();
    auto header = document.value(QStringLiteral("harkenDocument")).toObject();
    header.insert(QStringLiteral("propertyModel"), QStringLiteral("fieldGroups"));
    document.insert(QStringLiteral("harkenDocument"), header);
    const auto previousBytes = QJsonDocument(document).toJson();
    writeBytes(path, previousBytes);
    ProjectData destination;
    destination.name = QStringLiteral("Unchanged");
    bool upgradeApproved = false;
    ProjectSerializer reader;
    EXPECT_FALSE(reader.loadProject(path, destination, true, {}, [&](const auto&) {
        upgradeApproved = true;
        return true;
    }));
    EXPECT_FALSE(upgradeApproved);
    EXPECT_EQ(destination.name, QStringLiteral("Unchanged"));
    EXPECT_EQ(readBytes(path), previousBytes);
}

}
}
