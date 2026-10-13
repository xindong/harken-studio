#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Core/Components/VSelection.h"
#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectFormatVersion.h"
#include "ProjectManager.h"
#include "ProjectSchemaValidator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Support/ProjectDocumentTestUtils.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QTemporaryDir>
#include <QUuid>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::Platform;
using VNX::ProjectItem;
using VNX::VMixNode;

constexpr uint8_t kVolumeId = 1;
const QString kPlatformOverridesKey = QLatin1String(
    PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey);

ProjectItem makeDefinition(ProjectItem::Type type)
{
    ProjectItem item(QStringLiteral("definition"), type);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    return item;
}

QJsonObject overlayRowJson(const QString& platform, double volume)
{
    return QJsonObject{
        {QStringLiteral("platformId"), platform},
        {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
        {QStringLiteral("volume"), volume},
    };
}

void expectOverlayLoadRejected(const QJsonValue& value)
{
    VNX::PlatformOverrideLedgerV1 ledger;
    QString error;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::platformOverridesFromJson(
        value, &ledger, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(ledger.rows.empty());
}

TEST(PlatformOverrideSerialization, EmptyLedgerOmitsPlatformOverridesKey)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const QJsonObject definition =
        PlayablePropertyAuthoringV1::definitionToJson(sound);
    EXPECT_FALSE(definition.contains(kPlatformOverridesKey));

    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    auto child = std::make_shared<ProjectItem>(makeDefinition(ProjectItem::Type::Sound));
    child->parent = &container;
    container.children.push_back(child);
    const QUuid childGuid = child->guid;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        container, childGuid));
    const auto table = container.comp<VNX::VSelection>()->selections.shared.toJson();
    ASSERT_EQ(table.value(QStringLiteral("entries")).toArray().size(), 1);
    const auto row = table.value(QStringLiteral("entries")).toArray()[0].toObject();
    EXPECT_FALSE(row.contains(kPlatformOverridesKey));
    EXPECT_FALSE(row.contains(QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)));

    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = QUuid::createUuid();
    clip.duration = 1;
    EXPECT_FALSE(clip.toJson()
                     .value(QLatin1String(
                         harkenplayableproperties::studio::kEnvelopeJsonKeyV1))
                     .toObject()
                     .contains(kPlatformOverridesKey));
}

TEST(PlatformOverrideSerialization, DefinitionOverlayRoundtripsThroughEnvelope)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -3.0, HarkenTest::PlatformIds::iOS));
    const auto original = sound.platformOverrides;

    const QJsonObject envelope = PlayablePropertyAuthoringV1::definitionToJson(sound);
    ASSERT_TRUE(envelope.contains(kPlatformOverridesKey));
    const QJsonArray rows = envelope.value(kPlatformOverridesKey).toArray();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("platformId")).toString(),
              VNX::ProjectPlatformSemantics::idString(std::min(HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS)));
    EXPECT_EQ(rows.at(1).toObject().value(QStringLiteral("platformId")).toString(),
              VNX::ProjectPlatformSemantics::idString(std::max(HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS)));

    ProjectItem restored = makeDefinition(ProjectItem::Type::Sound);
    QString error;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::definitionFromJson(
        envelope, restored, &error));
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(restored.platformOverrides, original);
    EXPECT_EQ(restored.playablePropertyPresence.bits,
              sound.playablePropertyPresence.bits);
    EXPECT_FLOAT_EQ(restored.comp<VMixNode>()->volume, -6.0f);
}

TEST(PlatformOverrideSerialization, SelectionTablesPreservePlatformRowsAndRejectPlaybackParameters)
{
    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    auto child = std::make_shared<ProjectItem>(makeDefinition(ProjectItem::Type::Sound));
    child->parent = &container;
    container.children.push_back(child);
    const QUuid childGuid = child->guid;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        container, childGuid));
    auto& selections = container.comp<VNX::VSelection>()->selections;
    auto& platform = selections.editTable(HarkenTest::PlatformIds::Android);
    ASSERT_TRUE(platform.setOccurrenceRows({{childGuid, 0, 17}, {childGuid, 1, 83}}));
    platform.occurrenceOrdinalHighWater = 2;
    QString error;
    const auto json = selections.toJson();
    const auto restored = VNX::SelectionData::fromJson(json, &error);
    ASSERT_TRUE(restored) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(restored->toJson(), json);
    ASSERT_EQ(restored->shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(restored->shared.occurrenceRows().front().weight, 50);
    ASSERT_EQ(restored->table(HarkenTest::PlatformIds::Android).occurrenceRows().size(), 2u);
    EXPECT_EQ(restored->table(HarkenTest::PlatformIds::Android).occurrenceRows().back().weight, 83);
    auto invalid = platform.toJson();
    auto rows = invalid.value("entries").toArray();
    auto row = rows[0].toObject();
    row[QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] = QJsonObject{};
    rows[0] = row;
    invalid["entries"] = rows;
    EXPECT_FALSE(VNX::SelectionTable::fromJson(invalid, &error));
}

TEST(PlatformOverrideSerialization, ClipDiscardsPlaybackParametersAndPreservesTiming)
{
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = QUuid::createUuid();
    clip.start = 3;
    clip.duration = 8;
    clip.stableOccurrenceOrdinal = 7;
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(42));
    clip.playableProperties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    ASSERT_TRUE(clip.playableProperties.setValue(harkenplayableproperties::PlayablePropertyIdV1::Pitch, 150.0f));
    clip.platformOverrides.ensure(HarkenTest::PlatformIds::Android)->properties = clip.playableProperties;
    QJsonObject json = clip.toJson();
    EXPECT_FALSE(json.value(QLatin1String(
                    harkenplayableproperties::studio::kEnvelopeJsonKeyV1))
                    .toObject()
                    .contains(kPlatformOverridesKey));
    EXPECT_FALSE(json.value(QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1))
                     .toObject().contains(QStringLiteral("pitch")));
    auto legacyEnvelope = PlayablePropertyAuthoringV1::occurrenceToJson(clip.playableProperties);
    legacyEnvelope[kPlatformOverridesKey] =
        PlayablePropertyAuthoringV1::platformOverridesToJson(clip.platformOverrides);
    json[QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] = legacyEnvelope;
    QString error;
    const auto restored = VNX::Clip::fromJson(json, &error);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(error.isEmpty());
    EXPECT_TRUE(restored->platformOverrides.rows.empty());
    EXPECT_EQ(restored->playableProperties.presence.bits, 0u);
    EXPECT_EQ(restored->clipGuid, clip.clipGuid);
    EXPECT_EQ(restored->targetGuid, clip.targetGuid);
    EXPECT_EQ(restored->stableOccurrenceOrdinal, clip.stableOccurrenceOrdinal);
    EXPECT_EQ(restored->runtimeId(), clip.runtimeId());
    EXPECT_EQ(restored->start, clip.start);
    EXPECT_EQ(restored->duration, clip.duration);
}

TEST(PlatformOverrideSerialization, UnknownPlatformNameIsRejected)
{
    expectOverlayLoadRejected(QJsonArray{overlayRowJson(QStringLiteral("PS5"), -12.0)});
}

TEST(PlatformOverrideSerialization, UniversalPlatformNameIsRejected)
{
    expectOverlayLoadRejected(
        QJsonArray{overlayRowJson(QStringLiteral("Universal"), -12.0)});
}

TEST(PlatformOverrideSerialization, DuplicatePlatformRowsAreRejected)
{
    expectOverlayLoadRejected(QJsonArray{
        overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0),
        overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -3.0),
    });
}

TEST(PlatformOverrideSerialization, MixedPlatformFieldsAreRejected)
{
    auto row = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    row[QStringLiteral("platform")] = QStringLiteral("Android");
    expectOverlayLoadRejected(QJsonArray{row});
}

TEST(PlatformOverrideSerialization, InvertedJsonRowOrderIsSortedOnLoad)
{
    VNX::PlatformOverrideLedgerV1 ledger;
    QString error;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::platformOverridesFromJson(
        QJsonArray{
            overlayRowJson(HarkenTest::platformIdString(QStringLiteral("iOS")), -3.0),
            overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0),
        },
        &ledger,
        &error));
    EXPECT_TRUE(error.isEmpty());
    ASSERT_EQ(ledger.rows.size(), 2u);
    EXPECT_EQ(ledger.rows[0].platformId, std::min(HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS));
    EXPECT_EQ(ledger.rows[1].platformId, std::max(HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::iOS));
    EXPECT_TRUE(ledger.rows[0].platformId < ledger.rows[1].platformId);
}

TEST(PlatformOverrideSerialization, MissingOrNonStringPlatformIsRejected)
{
    QJsonObject missing = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    missing.remove(QStringLiteral("platformId"));
    expectOverlayLoadRejected(QJsonArray{missing});

    QJsonObject numeric = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    numeric[QStringLiteral("platformId")] = 1;
    expectOverlayLoadRejected(QJsonArray{numeric});
}

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QMap<QString, QByteArray> projectFileBytes(const QString& root)
{
    QMap<QString, QByteArray> out;
    const QDir rootDir(root);
    QDirIterator it(root,
                    QStringList() << QStringLiteral("*.hsp") << QStringLiteral("*.vp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        QFile f(abs);
        if (f.open(QIODevice::ReadOnly))
            out.insert(rootDir.relativeFilePath(abs), f.readAll());
    }
    return out;
}

QJsonObject harkenJsonIgnoringVolatileKeys(const QByteArray& data)
{
    QJsonObject root = QJsonDocument::fromJson(data).object();
    QJsonObject content = root[QStringLiteral("content")].toObject();
    QJsonObject metadata = content[QStringLiteral("metadata")].toObject();
    metadata.remove(QStringLiteral("modifiedDate"));
    content[QStringLiteral("metadata")] = metadata;
    root[QStringLiteral("content")] = content;
    return root;
}

TEST(PlatformOverrideSerialization, EmptyOverlayResaveIsByteStable)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.platforms = HarkenTest::platforms({"Windows", "Android", "iOS"});
    config.name = QStringLiteral("U2S3EmptyOverlay");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    ASSERT_TRUE(pm.createProject(config));
    VNX::ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;

    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("Shot"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    VNX::ProjectItem* container = pm.createItem(
        nullptr, QStringLiteral("Grp"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    VNX::ProjectItem* child = pm.createItem(
        container, QStringLiteral("Child"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    VNX::ProjectItem* track = pm.createItem(
        nullptr, QStringLiteral("Trk"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    ASSERT_TRUE(pm.moveItem(sound, track));
    clip.targetGuid = sound->guid;
    clip.duration = 4;
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);

    ASSERT_TRUE(pm.saveProject());
    const auto snap1 = projectFileBytes(projectRoot);
    ASSERT_FALSE(snap1.isEmpty());
    for (auto it = snap1.begin(); it != snap1.end(); ++it) {
        if (!it.key().endsWith(QStringLiteral(".vp"))) continue;
        EXPECT_FALSE(QString::fromUtf8(it.value()).contains(kPlatformOverridesKey))
            << it.key().toStdString();
    }

    ASSERT_TRUE(pm.loadProject(projectPath));
    ASSERT_TRUE(pm.saveProject());
    const auto snap2 = projectFileBytes(projectRoot);
    ASSERT_EQ(snap1.keys(), snap2.keys());
    for (auto it = snap1.begin(); it != snap1.end(); ++it) {
        if (it.key().endsWith(QStringLiteral(".hsp"))) {
            const QJsonObject first = harkenJsonIgnoringVolatileKeys(it.value());
            const QJsonObject second =
                harkenJsonIgnoringVolatileKeys(snap2.value(it.key()));
            EXPECT_EQ(first.keys(), second.keys());
            for (const QString& key : first.keys()) {
                if (key == QStringLiteral("formatVersion")
                    || key == QStringLiteral("schemaVersion")) {
                    EXPECT_EQ(first[key].toInt(), VNX::kCurrentProjectSchemaVersion);
                    EXPECT_EQ(second[key].toInt(), VNX::kCurrentProjectSchemaVersion);
                    continue;
                }
                EXPECT_EQ(first[key], second[key]) << key.toStdString();
            }
        } else {
            EXPECT_EQ(it.value(), snap2.value(it.key()))
                << it.key().toStdString();
        }
    }

    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();
}

TEST(PlatformOverrideSerialization, NumericOverlaySurvivesProjectManagerSaveReload)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.platforms = HarkenTest::platforms({"Windows", "Android", "iOS"});
    config.name = QStringLiteral("U2S12NumericOverlay");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    ASSERT_TRUE(pm.createProject(config));
    VNX::ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectPath = project->projectPath;

    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("Shot"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const QUuid soundGuid = sound->guid;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    QString saveError;
    const auto errorConn = QObject::connect(
        &pm, &VNX::ProjectManager::errorOccurred,
        [&saveError](const QString& error) { saveError = error; });
    ASSERT_TRUE(pm.saveProject()) << saveError.toStdString();
    QObject::disconnect(errorConn);
    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();

    ASSERT_TRUE(pm.loadProject(projectPath));
    VNX::ProjectItem* reloaded = pm.findItemByGuid(soundGuid);
    ASSERT_NE(reloaded, nullptr);
    const auto reloadedLocator =
        PlayablePropertyAuthoringV1::definitionLocator(*reloaded);
    ASSERT_TRUE(reloadedLocator.has_value());
    const auto android = PlayablePropertyAuthoringV1::readProperty(
        *reloaded, *reloadedLocator, kVolumeId, HarkenTest::PlatformIds::Android);
    ASSERT_TRUE(android.has_value());
    EXPECT_TRUE(android->present);
    EXPECT_DOUBLE_EQ(android->value.toDouble(), -12.0);
    const auto shared = PlayablePropertyAuthoringV1::readProperty(
        *reloaded, *reloadedLocator, kVolumeId, std::nullopt);
    ASSERT_TRUE(shared.has_value());
    EXPECT_TRUE(shared->present);
    EXPECT_DOUBLE_EQ(shared->value.toDouble(), -6.0);

    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();
}

TEST(PlatformOverrideSerialization, ReferenceAndSendsOverlaySurviveProjectManagerSaveReload)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.platforms = HarkenTest::platforms({"Windows", "Android", "iOS"});
    config.name = QStringLiteral("U2S12ReferenceOverlay");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    ASSERT_TRUE(pm.createProject(config));
    VNX::ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectPath = project->projectPath;

    VNX::ProjectItem* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    VNX::ProjectItem* targetBus = pm.createItem(
        master, QStringLiteral("SendTarget"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(targetBus, nullptr);
    VNX::ProjectItem* outputBus = pm.createItem(
        master, QStringLiteral("OutputTarget"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(outputBus, nullptr);
    VNX::ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("Shot"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const QUuid soundGuid = sound->guid;
    const QUuid outputGuid = outputBus->guid;
    const QUuid targetGuid = targetBus->guid;
    auto groups = sound->fieldGroups;
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(outputGuid), 0.0f}};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::iOS] = {
        VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{
            {VNX::toFieldGroupGuid(targetGuid), VMixNode::linearToDb(0.5f)}}}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, groups));

    QString saveError;
    const auto errorConn = QObject::connect(
        &pm, &VNX::ProjectManager::errorOccurred,
        [&saveError](const QString& error) { saveError = error; });
    ASSERT_TRUE(pm.saveProject()) << saveError.toStdString();
    QObject::disconnect(errorConn);
    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();

    ASSERT_TRUE(pm.loadProject(projectPath));
    VNX::ProjectItem* reloaded = pm.findItemByGuid(soundGuid);
    ASSERT_NE(reloaded, nullptr);
    VNX::ProjectItem* reloadedOutput = pm.findItemByGuid(outputGuid);
    ASSERT_NE(reloadedOutput, nullptr);
    VNX::ProjectItem* reloadedTarget = pm.findBusItemByName(QStringLiteral("SendTarget"));
    ASSERT_NE(reloadedTarget, nullptr);
    EXPECT_EQ(reloadedTarget->guid, targetGuid);
    EXPECT_EQ(reloaded->fieldGroups, groups);
    const auto& bus = reloaded->fieldGroups.mainOutput.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(bus.mode, VNX::FieldGroupMode::Local);
    ASSERT_TRUE(bus.localHistory);
    EXPECT_EQ(bus.localHistory->mainOutputBusRef,
              VNX::toFieldGroupGuid(reloadedOutput->guid));
    EXPECT_FLOAT_EQ(bus.localHistory->outputBusVolume, 0.0f);

    const auto& sends = reloaded->fieldGroups.send.platforms.at(
        HarkenTest::FieldGroupPlatformIds::iOS);
    EXPECT_EQ(sends.mode, VNX::FieldGroupMode::Local);
    ASSERT_TRUE(sends.localHistory);
    const auto& sendRows = sends.localHistory->sends;
    ASSERT_EQ(sendRows.size(), 1u);
    EXPECT_EQ(sendRows.front().targetBusID,
              VNX::toFieldGroupGuid(reloadedTarget->guid));
    EXPECT_NEAR(sendRows.front().sendLevel, VMixNode::linearToDb(0.5f), 1.0e-4);

    VNX::UndoRedoManager::instance().clear();
    pm.closeProject();
}

TEST(PlatformOverrideSerialization, UniversalOverlayRowIsRejectedBySchemaAndLoader)
{
    const QJsonArray universal{
        overlayRowJson(QStringLiteral("Universal"), -12.0),
    };
    expectOverlayLoadRejected(universal);

    const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject content{
        {QStringLiteral("type"), QStringLiteral("Sound")},
        {QStringLiteral("guid"), guid},
        {QStringLiteral("name"), QStringLiteral("Shot")},
        {QStringLiteral("parentRef"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("root")},
        }},
        {QStringLiteral("children"), QJsonArray{}},
        {QStringLiteral("order"), 0},
        {QStringLiteral("playableProperties"), QJsonObject{
            {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
            {kPlatformOverridesKey, universal},
        }},
    };
    const QStringList schemaErrors =
        VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::kCurrentProjectSchemaVersion,
            QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                                       HarkenTest::kProjectId, content),
            QStringLiteral("Layers/Shot.vp"));
    EXPECT_FALSE(schemaErrors.isEmpty());
}

TEST(PlatformOverrideSerialization, UnknownOverlayPlatformIsRejectedBySchemaAndLoader)
{
    const QJsonArray unknown{
        overlayRowJson(QStringLiteral("PS5"), -12.0),
    };
    expectOverlayLoadRejected(unknown);

    const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject content{
        {QStringLiteral("type"), QStringLiteral("Sound")},
        {QStringLiteral("guid"), guid},
        {QStringLiteral("name"), QStringLiteral("Shot")},
        {QStringLiteral("parentRef"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("root")},
        }},
        {QStringLiteral("children"), QJsonArray{}},
        {QStringLiteral("order"), 0},
        {QStringLiteral("playableProperties"), QJsonObject{
            {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
            {kPlatformOverridesKey, unknown},
        }},
    };
    const QStringList schemaErrors =
        VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::kCurrentProjectSchemaVersion,
            QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                                       HarkenTest::kProjectId, content),
            QStringLiteral("Layers/Shot.vp"));
    EXPECT_FALSE(schemaErrors.isEmpty());
}

}  // namespace
