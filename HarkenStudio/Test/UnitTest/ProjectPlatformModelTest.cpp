#include "Core/Components/VSelection.h"
#include "Core/ProjectPlatform.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectContract.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/Components/VTrack.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/PlatformContextService.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "Support/ProjectPlatformTestUtils.h"

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

QMap<QString, QByteArray> authoredBytes(const QString& root)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                       QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) result.insert(path, file.readAll());
    }
    return result;
}

void writeJson(const QString& path, const QJsonObject& json)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto bytes = QJsonDocument(json).toJson();
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QVector<ProjectPlatform> androidPlatforms()
{
    return {{QUuid::createUuid(), QStringLiteral("Android_Low"), BasePlatform::Android},
            {QUuid::createUuid(), QStringLiteral("Android_High"), BasePlatform::Android}};
}

class ProjectPlatformModel : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        if (ProjectManager::instance().isProjectOpen()) ProjectManager::instance().closeProject();
        ASSERT_TRUE(directory.isValid());
        config.name = QStringLiteral("PlatformModel");
        config.location = directory.path();
    }

    void TearDown() override
    {
        if (ProjectManager::instance().isProjectOpen()) ProjectManager::instance().closeProject();
    }

    QTemporaryDir directory;
    ProjectConfig config;
};

TEST_F(ProjectPlatformModel, BothCreationPathsPreserveDefaultPlatformInstances)
{
    ASSERT_EQ(config.platforms.size(), 4);
    const QVector<BasePlatform> expectedBases{
        BasePlatform::Windows, BasePlatform::Android, BasePlatform::macOS, BasePlatform::iOS};
    QVector<ProjectPlatformId> expectedIds;
    for (qsizetype index = 0; index < config.platforms.size(); ++index) {
        const auto& platform = config.platforms.at(index);
        EXPECT_EQ(platform.basePlatform, expectedBases.at(index));
        EXPECT_EQ(platform.name, Platforms::basePlatformName(expectedBases.at(index)));
        expectedIds.append(platform.id);
    }
    EXPECT_TRUE(Platforms::validatePlatforms(config.platforms));
    const auto expected = Platforms::platformsToJson(config.platforms);
    const auto id = config.platforms.first().id;
    EXPECT_TRUE(ProjectData{}.platforms.isEmpty());
    auto prepared = ProjectManager::prepareProject(config);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    EXPECT_EQ(Platforms::platformsToJson(prepared.project->platforms), expected);
    ASSERT_TRUE(ProjectManager::instance().commitPreparedProject(std::move(prepared)));
    EXPECT_EQ(ProjectManager::instance().getPackExportCurrentPlatform(), Platforms::idString(id));
    EXPECT_EQ(ProjectManager::instance().getPackExportSelectedPlatforms(), Platforms::ids(config.platforms));
    ProjectManager::instance().closeProject();
    config.name = QStringLiteral("Ordinary");
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.createProject(config));
    EXPECT_EQ(Platforms::platformsToJson(manager.currentProject()->platforms), expected);
    EXPECT_EQ(manager.getPackExportCurrentPlatform(), Platforms::idString(id));
    EXPECT_EQ(PlatformContextService::instance().context().primary, id);
    EXPECT_EQ(PlatformContextService::instance().context().selected, expectedIds);
    EXPECT_NE(ProjectConfig{}.platforms.first().id, id);
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_EQ(Platforms::platformsToJson(manager.currentProject()->platforms), expected);
}

TEST(ProjectPlatformSemantics, InstancesStoreIndependentBaseDefaultsAndRejectRetiredProfiles)
{
    for (const auto base : {BasePlatform::Windows, BasePlatform::macOS, BasePlatform::Linux,
                           BasePlatform::Android, BasePlatform::iOS}) {
        ProjectPlatform first{QUuid::createUuid(), QStringLiteral("First"), base};
        ProjectPlatform second{QUuid::createUuid(), QStringLiteral("Second"), base};
        const auto defaults = ProjectPlatformProfile::fromBasePlatform(base).toJson();
        EXPECT_EQ(first.profile.toJson(), defaults);
        EXPECT_EQ(first.toJson().value("profile").toObject(), defaults);
        first.profile.sizePolicies.front().config.targetSampleRate = 32000;
        first.profile.performResample = true;
        EXPECT_EQ(second.profile.toJson(), defaults);
        AssetInfo source;
        source.metadata.fileSize = 4096;
        EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(source, first).targetSampleRate, 32000u);
        QVector<ProjectPlatform> parsed;
        ASSERT_TRUE(Platforms::platformsFromJson(QJsonArray{first.toJson()}, &parsed));
        EXPECT_EQ(parsed.first().toJson(), first.toJson());
        const auto valid = parsed;
        auto missing = first.toJson();
        missing.remove("profile");
        EXPECT_FALSE(Platforms::platformsFromJson(QJsonArray{missing}, &parsed));
        for (const auto* key : {"normalize", "normalizeTargetDb", "forceMono", "trimSilence", "trimSilenceThresholdDb"}) {
            auto row = first.toJson();
            auto profile = row.value("profile").toObject();
            auto policies = profile.value("sizePolicies").toArray();
            auto policy = policies.first().toObject();
            auto config = policy.value("config").toObject();
            config.insert(key, key == QStringLiteral("normalize") ? QJsonValue(true) : QJsonValue(-1.0));
            policy.insert("config", config);
            policies[0] = policy;
            profile.insert("sizePolicies", policies);
            row.insert("profile", profile);
            EXPECT_FALSE(Platforms::platformsFromJson(QJsonArray{row}, &parsed));
        }
        EXPECT_EQ(Platforms::platformsToJson(parsed), Platforms::platformsToJson(valid));
    }
}

TEST(ProjectPlatformSemantics, SizeLimitsRejectDuplicatesAndInvalidBoundsAtomically)
{
    auto definitions = androidPlatforms();
    const auto before = Platforms::platformsToJson(definitions);
    const std::vector<std::vector<SizePolicy>> invalid{
        {}, {{512, {}}}, {{512, {}}, {512, {}}, {std::nullopt, {}}},
        {{1024, {}}, {512, {}}, {std::nullopt, {}}},
        {{std::nullopt, {}}, {512, {}}}, {{std::nullopt, {}}, {std::nullopt, {}}},
        {{UINT64_MAX, {}}, {std::nullopt, {}}}
    };
    for (const auto& rows : invalid) {
        auto candidate = definitions;
        candidate.front().profile.sizePolicies = rows;
        EXPECT_FALSE(Platforms::validatePlatforms(candidate));
        EXPECT_FALSE(Platforms::platformsFromJson(Platforms::platformsToJson(candidate), &definitions));
        EXPECT_EQ(Platforms::platformsToJson(definitions), before);
    }
    auto valid = definitions;
    valid.front().profile.sizePolicies = {{0, {}}, {uint64_t(INT64_MAX), {}}, {std::nullopt, {}}};
    const auto encoded = QJsonDocument(Platforms::platformsToJson(valid)).toJson();
    ASSERT_TRUE(Platforms::platformsFromJson(QJsonDocument::fromJson(encoded).array(), &definitions));
    EXPECT_EQ(Platforms::platformsToJson(definitions), Platforms::platformsToJson(valid));
    for (const auto& bad : {QJsonValue(-1), QJsonValue(1.5), QJsonValue("512"), QJsonValue(9223372036854775808.0)}) {
        auto row = valid.front().profile.sizePolicies.front().toJson();
        row.insert("maxBytes", bad);
        auto platform = valid.front().toJson();
        platform.insert("profile", QJsonObject{{"performResample", false},
            {"sizePolicies", QJsonArray{row, valid.front().profile.sizePolicies.back().toJson()}}});
        EXPECT_FALSE(Platforms::platformsFromJson(QJsonArray{platform}, &definitions));
        EXPECT_EQ(Platforms::platformsToJson(definitions), Platforms::platformsToJson(valid));
    }
    for (int legacy = 0; legacy < 3; ++legacy) {
        auto platform = valid.front().toJson();
        auto profile = platform.value("profile").toObject();
        auto policies = profile.value("sizePolicies").toArray();
        auto row = policies.first().toObject();
        if (legacy == 0) profile.remove("performResample");
        if (legacy == 1) row.insert("minBytes", 0);
        if (legacy == 2) {
            auto config = row.value("config").toObject();
            config.insert("performResample", true);
            row.insert("config", config);
        }
        policies[0] = row;
        profile.insert("sizePolicies", policies);
        platform.insert("profile", profile);
        EXPECT_FALSE(Platforms::platformsFromJson(QJsonArray{platform}, &definitions));
        EXPECT_EQ(Platforms::platformsToJson(definitions), Platforms::platformsToJson(valid));
    }
}

TEST_F(ProjectPlatformModel, InvalidCreationDoesNotCreateDirectory)
{
    config.platforms.clear();
    EXPECT_FALSE(ProjectManager::instance().createProject(config));
    EXPECT_EQ(ProjectManager::prepareProject(config).project, nullptr);
    EXPECT_FALSE(QDir(directory.path()).exists(config.name));
}

TEST(ProjectPlatformSemantics, BaseTypesAndIdentifiersAreStrict)
{
    for (const auto& name : {"Windows", "macOS", "Linux", "Android", "iOS"})
        EXPECT_TRUE(Platforms::parseBasePlatform(QLatin1String(name)));
    for (const auto& name : {"Universal", "Desktop", "android", " Android", "Android_Low"})
        EXPECT_FALSE(Platforms::parseBasePlatform(QLatin1String(name)));
    const auto id = QUuid::createUuid();
    EXPECT_EQ(Platforms::parseId(Platforms::idString(id)), id);
    EXPECT_FALSE(Platforms::parseId(id.toString()));
    EXPECT_FALSE(Platforms::parseId(QStringLiteral("00000000-0000-0000-0000-000000000000")));
    EXPECT_FALSE(Platforms::parseId(QStringLiteral("Android")));
}

TEST(ProjectPlatformSemantics, NamesAndDefinitionIdentityAreValidated)
{
    for (const auto& name : {"", ".", "..", "../Android", "A/B", "A\\B", "A:", "A?", "A*", "A|",
                             "A\nB", "A.", "A ", "CON", "con.txt", "LPT9", "COM1.data"})
        EXPECT_FALSE(Platforms::validateName(QLatin1String(name))) << name;
    EXPECT_FALSE(Platforms::validateName(QString(121, QChar('a'))));
    EXPECT_TRUE(Platforms::validateName(QString::fromUtf8("\xe5\xae\x89\xe5\x8d\x93")));
    auto platforms = androidPlatforms();
    EXPECT_TRUE(Platforms::validatePlatforms(platforms));
    platforms[1].id = platforms[0].id;
    EXPECT_FALSE(Platforms::validatePlatforms(platforms));
    platforms[1].id = QUuid::createUuid();
    platforms[1].name = platforms[0].name.toUpper();
    EXPECT_FALSE(Platforms::validatePlatforms(platforms));
    platforms[0].name = QString::fromUtf8("Caf\xc3\xa9");
    platforms[1].name = QStringLiteral("Cafe") + QChar(0x301);
    EXPECT_FALSE(Platforms::validatePlatforms(platforms));
}

TEST(ProjectPlatformSemantics, MalformedProfilesDoNotReplaceExistingDefinitions)
{
    auto definitions = androidPlatforms();
    const auto before = Platforms::platformsToJson(definitions);
    auto withProfile = definitions.first();
    for (const auto& field : {QStringLiteral("format"), QStringLiteral("vorbisEncodeMode")}) {
        auto row = withProfile.toJson();
        auto profile = row[QStringLiteral("profile")].toObject();
        auto policies = profile.value("sizePolicies").toArray();
        auto policy = policies.first().toObject();
        auto settings = policy.value("config").toObject();
        settings[field] = static_cast<qint64>(UINT32_MAX);
        policy.insert("config", settings);
        policies[0] = policy;
        profile.insert("sizePolicies", policies);
        row[QStringLiteral("profile")] = profile;
        EXPECT_FALSE(Platforms::platformsFromJson(QJsonArray{row}, &definitions));
        EXPECT_EQ(Platforms::platformsToJson(definitions), before);
    }
}

TEST_F(ProjectPlatformModel, TwoAndroidInstancesRoundTripEveryAuthoredReference)
{
    config.platforms = androidPlatforms();
    config.platforms[0].profile.sizePolicies.front().config.targetSampleRate = 22050;
    config.platforms[0].profile.performResample = true;
    config.platforms[0].profile.sizePolicies.front().maxBytes = 1024;
    config.platforms[0].profile.sizePolicies.push_back({std::nullopt, ConversionParameters{}});
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.createProject(config));
    auto* project = manager.currentProject();
    const auto path = project->projectPath;
    const auto low = config.platforms[0].id;
    const auto high = config.platforms[1].id;
    const auto lowString = Platforms::idString(low);
    const auto highString = Platforms::idString(high);
    auto* track = manager.createItem(nullptr, QStringLiteral("Track"), ProjectItem::Type::Track);
    auto* sound = manager.createItem(track, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    auto* container = manager.createItem(nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    auto* child = manager.createItem(container, QStringLiteral("Child"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(track, nullptr);
    const auto soundId = sound->guid;
    const auto containerId = container->guid;
    const auto trackId = track->guid;
    const auto propertyId = harkenplayableproperties::PlayablePropertyIdV1::Volume;
    for (auto* ledger : {&sound->platformOverrides}) {
        ASSERT_TRUE(ledger->ensure(low)->properties.setValue(propertyId, 0.25f));
        ASSERT_TRUE(ledger->ensure(high)->properties.setValue(propertyId, 0.75f));
        ledger->ensure(low)->properties.presence.Set(propertyId);
        ledger->ensure(high)->properties.presence.Set(propertyId);
    }
    container->comp<VNX::VSelection>()->selections.editTable(low).entries.front().occurrences.front().weight = 25;
    container->comp<VNX::VSelection>()->selections.editTable(high).entries.front().occurrences.front().weight = 75;
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = soundId;
    clip.duration = 4;
    ASSERT_TRUE(clip.platformOverrides.ensure(low)->properties.setValue(propertyId, 0.25f));
    ASSERT_TRUE(clip.platformOverrides.ensure(high)->properties.setValue(propertyId, 0.75f));
    clip.platformOverrides.ensure(low)->properties.presence.Set(propertyId);
    clip.platformOverrides.ensure(high)->properties.presence.Set(propertyId);
    auto* timeline = track->comp<VTrack>();
    timeline->clips.items.clear();
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto firstEntry = QUuid::createUuid();
    const auto secondEntry = QUuid::createUuid();
    auto* sources = sound->comp<VSound>();
    sources->sources.plain.entries = {{firstEntry, 11}, {secondEntry, 12}};
    sources->sources.plain.platforms[lowString].active = firstEntry;
    sources->sources.plain.platforms[highString].active = secondEntry;
    sources->sources.plain.active = firstEntry;
    auto* action = manager.createItem(nullptr, QStringLiteral("Action"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    manager.setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    ProjectManager::instance().setActionPackTargetingInternal({{action->runtimeId(), false, {highString}},
                                    {action->runtimeId(), false, {lowString}, QStringLiteral("English")},
                                    {action->runtimeId(), false, {}, QStringLiteral("Japanese")}});
    ProjectData::DeploymentProfile deployment;
    deployment.profileId = QStringLiteral("test");
    deployment.name = QStringLiteral("Device");
    deployment.engineKind = QStringLiteral("unity");
    deployment.platformIds = {highString};
    project->deploymentProfiles = {deployment};
    ProjectManager::instance().setPackExportCurrentPlatformInternal(highString);
    ProjectManager::instance().setPackExportSelectedPlatformsInternal({highString, lowString});
    QString error;
    ASSERT_TRUE(Platforms::validateProject(*project, &error)) << error.toStdString();
    ASSERT_TRUE(manager.saveProject());
    const auto platformsJson = Platforms::platformsToJson(project->platforms);
    manager.closeProject();
    auto reopened = ProjectManager::prepareProject(path);
    ASSERT_NE(reopened.project, nullptr) << reopened.error.toStdString();
    ASSERT_TRUE(manager.commitPreparedProject(std::move(reopened)));
    project = manager.currentProject();
    EXPECT_EQ(Platforms::platformsToJson(project->platforms), platformsJson);
    EXPECT_EQ(ProjectManager::instance().getPackExportCurrentPlatform(), highString);
    EXPECT_EQ(ProjectManager::instance().getPackExportSelectedPlatforms(), (QStringList{highString, lowString}));
    EXPECT_EQ(ProjectManager::instance().getActionPackTargeting()[1].platformIds, QStringList{lowString});
    EXPECT_FALSE(ProjectManager::instance().getActionPackTargeting()[2].allPlatforms);
    EXPECT_TRUE(ProjectManager::instance().getActionPackTargeting()[2].platformIds.isEmpty());
    EXPECT_EQ(ProjectManager::instance().getActionPackTargeting()[0].platformIds, QStringList{highString});
    EXPECT_EQ(project->deploymentProfiles[0].platformIds, QStringList{highString});
    sound = manager.findItemByGuid(soundId);
    container = manager.findItemByGuid(containerId);
    track = manager.findItemByGuid(trackId);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(sound->platformOverrides.find(low), nullptr);
    ASSERT_NE(sound->platformOverrides.find(high), nullptr);
    EXPECT_NEAR(std::get<float>(*sound->platformOverrides.find(low)->properties.value(propertyId)), 0.25f, 0.00001f);
    EXPECT_NEAR(std::get<float>(*sound->platformOverrides.find(high)->properties.value(propertyId)), 0.75f, 0.00001f);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.table(low).occurrenceRows().front().weight, 25);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.table(high).occurrenceRows().front().weight, 75);
    timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    EXPECT_EQ(timeline->clips.items.front().clipGuid, clip.clipGuid);
    EXPECT_EQ(timeline->clips.items.front().targetGuid, soundId);
    EXPECT_EQ(timeline->clips.items.front().duration, clip.duration);
    EXPECT_TRUE(timeline->clips.items.front().platformOverrides.rows.empty());
    sources = sound->comp<VSound>();
    const auto lowSource = SoundSourceSemantics::resolveSourceSelection(*sources, lowString, {});
    const auto highSource = SoundSourceSemantics::resolveSourceSelection(*sources, highString, {});
    ASSERT_NE(lowSource.source, nullptr);
    ASSERT_NE(highSource.source, nullptr);
    EXPECT_EQ(lowSource.source->entryId, firstEntry);
    EXPECT_EQ(highSource.source->entryId, secondEntry);
    manager.closeProject();
    const auto saved = authoredBytes(directory.path());
    bool corrupted = false;
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        if (!it.key().endsWith(QStringLiteral(".vp")) || !it.value().contains(lowString.toUtf8())) continue;
        QFile file(it.key());
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        auto bytes = it.value();
        bytes.replace(lowString.toUtf8(), Platforms::idString(QUuid::createUuid()).toUtf8());
        ASSERT_EQ(file.write(bytes), bytes.size());
        corrupted = true;
        break;
    }
    ASSERT_TRUE(corrupted);
    const auto beforeOpen = authoredBytes(directory.path());
    const auto rejected = ProjectManager::prepareProject(path);
    EXPECT_EQ(rejected.project, nullptr);
    EXPECT_TRUE(rejected.error.contains(QStringLiteral("platform ID"))) << rejected.error.toStdString();
    EXPECT_EQ(authoredBytes(directory.path()), beforeOpen);
}

TEST_F(ProjectPlatformModel, InvalidPersistedTablesAndRootReferencesAreRejected)
{
    config.platforms = androidPlatforms();
    const auto prepared = ProjectManager::prepareProject(config);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    const auto path = prepared.project->projectPath;
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    for (int scenario = 0; scenario < 9; ++scenario) {
        auto document = original;
        auto content = document[QStringLiteral("content")].toObject();
        auto metadata = content[QStringLiteral("metadata")].toObject();
        auto platforms = metadata[QStringLiteral("platforms")].toArray();
        auto first = platforms[0].toObject();
        auto second = platforms[1].toObject();
        if (scenario == 0) first[QStringLiteral("id")] = QJsonValue::Null;
        if (scenario == 1) second[QStringLiteral("id")] = first[QStringLiteral("id")];
        if (scenario == 2) second[QStringLiteral("name")] = first[QStringLiteral("name")].toString().toUpper();
        if (scenario == 3) first[QStringLiteral("basePlatform")] = QStringLiteral("Universal");
        if (scenario == 4) first[QStringLiteral("name")] = QStringLiteral("../Outside");
        if (scenario == 5) content[QStringLiteral("packExportCurrentPlatformId")] = Platforms::idString(QUuid::createUuid());
        if (scenario == 6) content[QStringLiteral("packExportSelectedPlatformIds")] = QJsonArray{first[QStringLiteral("id")], first[QStringLiteral("id")]};
        if (scenario == 7) first[QStringLiteral("profile")] = QJsonObject{{QStringLiteral("defaultConfig"), QJsonObject{}}};
        platforms[0] = first;
        platforms[1] = second;
        if (scenario == 8) platforms = {};
        metadata[QStringLiteral("platforms")] = platforms;
        content[QStringLiteral("metadata")] = metadata;
        document[QStringLiteral("content")] = content;
        writeJson(path, document);
        const auto before = authoredBytes(directory.path());
        if (scenario == 5 || scenario == 6)
            EXPECT_NE(ProjectManager::prepareProject(path).project, nullptr) << scenario;
        else
            EXPECT_EQ(ProjectManager::prepareProject(path).project, nullptr) << scenario;
        EXPECT_EQ(authoredBytes(directory.path()), before) << scenario;
    }
}

TEST_F(ProjectPlatformModel, HardCutRejectsLegacyAndMixedRootsWithoutWriting)
{
    auto prepared = ProjectManager::prepareProject(config);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    const auto path = prepared.project->projectPath;
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    for (bool mixed : {false, true}) {
        auto document = original;
        auto content = document[QStringLiteral("content")].toObject();
        auto metadata = content[QStringLiteral("metadata")].toObject();
        if (!mixed) metadata.remove(QStringLiteral("platforms"));
        metadata.insert(QStringLiteral("targetPlatforms"), QJsonArray{QStringLiteral("Android")});
        content.insert(QStringLiteral("metadata"), metadata);
        document.insert(QStringLiteral("content"), content);
        writeJson(path, document);
        const auto before = authoredBytes(directory.path());
        const auto analysis = ProjectUpgradeManager{}.analyze(path);
        EXPECT_FALSE(analysis.ok);
        EXPECT_FALSE(analysis.upgradeRequired);
        EXPECT_TRUE(analysis.blockingErrors.join(QChar(' ')).contains(QStringLiteral("Legacy")));
        EXPECT_EQ(ProjectManager::prepareProject(path).project, nullptr);
        EXPECT_EQ(authoredBytes(directory.path()), before);
    }
}

TEST_F(ProjectPlatformModel, InvalidCachedPlatformsDoNotBlockProjectSave)
{
    ASSERT_TRUE(ProjectManager::instance().createProject(config));
    auto* project = ProjectManager::instance().currentProject();
    ProjectManager::instance().setPackExportSelectedPlatformsInternal({Platforms::idString(QUuid::createUuid())});
    EXPECT_TRUE(ProjectManager::instance().getPackExportSelectedPlatforms().isEmpty());
    EXPECT_TRUE(ProjectManager::instance().saveProject());
    ProjectSerializer serializer;
    EXPECT_TRUE(serializer.saveProject(project->projectPath, *project));
}

TEST(ProjectPlatformSemantics, NestedAndSharedSelectionReferencesAreChecked)
{
    auto project = HarkenTest::projectWithPlatforms();
    const auto id = project.platformIds().first();
    const auto unknown = Platforms::idString(QUuid::createUuid());
    const QJsonObject root{{QStringLiteral("metadata"), QJsonObject{{QStringLiteral("platforms"), Platforms::platformsToJson(project.platforms)}}},
                           {QStringLiteral("packExportCurrentPlatformId"), id},
                           {QStringLiteral("packExportSelectedPlatformIds"), QJsonArray{id}}};
    for (const auto& key : {"platformOverrides"}) {
        const QJsonObject row{{QStringLiteral("platformId"), unknown}};
        const QJsonObject nested{{QStringLiteral("rows"), QJsonArray{QJsonObject{{QLatin1String(key), QJsonArray{row}}}}}};
        EXPECT_FALSE(Platforms::validateDocuments(root, {{QStringLiteral("Layers/Test.vp"), nested}}).isEmpty());
    }
    VSound sound;
    sound.sources.localized["English"].platforms[unknown].active = QUuid{};
    EXPECT_FALSE(Platforms::validateDocuments(root, {{"Sound.vp", sound.toJson()}}).isEmpty());
    sound.sources.localized["English"].platforms.clear();
    const QJsonObject shared = sound.toJson();
    EXPECT_TRUE(Platforms::validateDocuments(root, {{QStringLiteral("Sound.vp"), shared}}).isEmpty());
    const QJsonObject duplicate{{QStringLiteral("platformOverrides"), QJsonArray{
        QJsonObject{{QStringLiteral("platformId"), id}}, QJsonObject{{QStringLiteral("platformId"), id}}}}};
    EXPECT_FALSE(Platforms::validateDocuments(root, {{QStringLiteral("Track.vp"), duplicate}}).isEmpty());
}

TEST(ProjectPlatformSemantics, ActiveOverridesRequireCanonicalPlatformKeys)
{
    SoundSources sources;
    sources.plain.entries = {{QUuid::createUuid(), 11}};
    sources.plain.active = sources.plain.entries[0].entryId;
    EXPECT_TRUE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
    for (const auto& value : {QString(), QStringLiteral("Universal"), QStringLiteral("Android")}) {
        sources.plain.platforms.clear();
        sources.plain.platforms[value].active = sources.plain.active;
        EXPECT_FALSE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
    }
}
}
