#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/BindingSemantics.h"
#include "Core/ActionManager.h"
#include "Core/ActionStepEditing.h"
#include "Core/EffectChainSemantics.h"
#include "Core/Components/VConversion.h"
#include "Core/NameValidator.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSegment.h"
#include "Core/Components/VSelection.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectContract.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace {

using namespace harken::studio;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

QJsonObject readObject(const QString& path)
{
    const QJsonDocument document = QJsonDocument::fromJson(readFile(path));
    EXPECT_TRUE(document.isObject()) << path.toStdString();
    return document.object();
}

void writeObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QMap<QString, QString> documentPathsById(const QString& projectDirectory)
{
    QMap<QString, QString> paths;
    QDirIterator files(projectDirectory,
                       QStringList{QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                       QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString id = HarkenTest::projectHeader(readObject(path))
                               .value(QStringLiteral("id")).toString();
        EXPECT_FALSE(id.isEmpty()) << path.toStdString();
        EXPECT_FALSE(paths.contains(id)) << id.toStdString();
        paths.insert(id, path);
    }
    return paths;
}

QJsonObject legacyConversionParameters(int rate)
{
    return {{QStringLiteral("format"), 1}, {QStringLiteral("targetSampleRate"), rate},
        {QStringLiteral("vorbisAverageBitrate"), 0}, {QStringLiteral("vorbisEncodeMode"), 0},
        {QStringLiteral("vorbisMaxBitrate"), 0}, {QStringLiteral("vorbisMinBitrate"), 0},
        {QStringLiteral("vorbisQuality"), 0.6}};
}

QJsonObject legacyProfile(bool resample, const QJsonArray& policies = {})
{
    return {{QStringLiteral("performResample"), resample},
        {QStringLiteral("sizePolicies"), policies.isEmpty() ? QJsonArray{QJsonObject{
            {QStringLiteral("maxBytes"), QJsonValue::Null},
            {QStringLiteral("config"), legacyConversionParameters(48000)}}} : policies}};
}

void removeConversionGroup(QJsonObject& content)
{
    if (content.value(QStringLiteral("fieldGroups")).isObject()) {
        auto groups = content.value(QStringLiteral("fieldGroups")).toObject();
        auto shared = groups.value(QStringLiteral("shared")).toObject();
        shared.remove(QStringLiteral("conversion"));
        groups.insert(QStringLiteral("shared"), shared);
        if (groups.value(QStringLiteral("platforms")).isObject()) {
            auto platforms = groups.value(QStringLiteral("platforms")).toObject();
            for (const auto& key : platforms.keys()) {
                auto platform = platforms.value(key).toObject();
                platform.remove(QStringLiteral("conversion"));
                if (platform.isEmpty()) platforms.remove(key);
                else platforms.insert(key, platform);
            }
            if (platforms.isEmpty()) groups.remove(QStringLiteral("platforms"));
            else groups.insert(QStringLiteral("platforms"), platforms);
        }
        content.insert(QStringLiteral("fieldGroups"), groups);
    }
    if (content.value(QStringLiteral("children")).isArray()) {
        auto children = content.value(QStringLiteral("children")).toArray();
        for (qsizetype index = 0; index < children.size(); ++index) {
            auto child = children[index].toObject();
            removeConversionGroup(child);
            children[index] = child;
        }
        content.insert(QStringLiteral("children"), children);
    }
}

QJsonObject extensionMetadata(const QString& id)
{
    return {
        {QStringLiteral("futureAuthorNotes"), QJsonObject{
            {QStringLiteral("blob"), QString(4096, QLatin1Char('a'))},
            {QStringLiteral("document"), id},
            {QStringLiteral("values"), QJsonArray{
                QJsonValue::Null, true, QJsonValue(qint64(9007199254740993LL)),
                -2.5, QStringLiteral("Unedited metadata"), QJsonObject{}, QJsonArray{},
                QJsonObject{{QStringLiteral("type"), QStringLiteral("Piece")},
                    {QStringLiteral("pieceTime"), QStringLiteral("piecetimeline")}},
            }},
        }},
    };
}

ProjectItem* findItem(ProjectData& project, const QUuid& guid)
{
    const auto find = [&](auto&& self, ProjectItem* item) -> ProjectItem* {
        if (!item) return nullptr;
        if (item->guid == guid) return item;
        for (const auto& child : item->children)
            if (ProjectItem* found = self(self, child.get())) return found;
        return nullptr;
    };
    for (ProjectFileBinding* binding : project.allFileBindings())
        if (ProjectItem* item = find(find, binding->rootItem.get())) return item;
    return nullptr;
}

TEST(ProjectDocumentModuleData, KnownBindingAndEffectEditsPreserveRawModuleFields)
{
    const QJsonObject futureData = extensionMetadata(QStringLiteral("module"));
    ParameterBindingDef binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = harken::studio::authoringUuid(harkentest::StudioObjectId(41));
    binding.address.path = BindingPath{3};
    QJsonObject bindingJson = binding.toJson().toObject();
    QJsonObject address = bindingJson.value(QStringLiteral("address")).toObject();
    address.insert(QStringLiteral("futureAddressData"), futureData);
    bindingJson.insert(QStringLiteral("address"), address);
    ParameterBindingDef loadedBinding = ParameterBindingDef::fromJson(bindingJson);
    ASSERT_TRUE(loadedBinding.typed);
    loadedBinding.outputMax = 12.0f;
    loadedBinding.address.path.reset();
    const QJsonObject savedBinding = loadedBinding.toJson().toObject();
    const QJsonObject savedAddress = savedBinding.value(QStringLiteral("address")).toObject();
    EXPECT_EQ(savedAddress.value(QStringLiteral("futureAddressData")), futureData);
    const ParameterBindingDef reopenedBinding = ParameterBindingDef::fromJson(savedBinding);
    ASSERT_TRUE(reopenedBinding.typed);
    EXPECT_EQ(reopenedBinding.addressExtensions.value(QStringLiteral("futureAddressData")),
              futureData);

    const QJsonObject effectJson{
        {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("effectGuid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("bypass"), true},
        {QStringLiteral("futureEffectData"), futureData},
    };
    EffectEntry effect = EffectEntry::fromJson(effectJson);
    ASSERT_TRUE(effect.typed);
    effect.bypass = false;
    const QJsonObject savedEffect = effect.toJson().toObject();
    EXPECT_EQ(savedEffect.value(QStringLiteral("futureEffectData")), futureData);
    const EffectEntry reopenedEffect = EffectEntry::fromJson(savedEffect);
    ASSERT_TRUE(reopenedEffect.typed);
    EXPECT_EQ(reopenedEffect.extensions.value(QStringLiteral("futureEffectData")),
              futureData);
}

class ProjectDocumentRoundTripTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char executable[] = "harken_studio_tests";
            static char* argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
        ASSERT_TRUE(directory_.isValid());
        auto& manager = ProjectManager::instance();
        ProjectConfig config;
        config.name = QStringLiteral("MetadataRoundTrip");
        config.location = directory_.path();
        ASSERT_TRUE(manager.createProject(config));
        auto* layer = manager.createItem(
            nullptr, QStringLiteral("Main"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        auto* childLayer = manager.createItem(
            layer, QStringLiteral("Nested"), ProjectItem::Type::Layer);
        ASSERT_NE(childLayer, nullptr);
        ASSERT_NE(manager.createItem(childLayer, QStringLiteral("Tone"),
                                     ProjectItem::Type::Sound), nullptr);
        auto* segment = manager.createItem(layer, QStringLiteral("Piece"),
                                           ProjectItem::Type::Segment);
        ASSERT_NE(segment, nullptr);
        segmentGuid_ = segment->guid;
        auto* container = manager.createItem(layer, QStringLiteral("Choice"), ProjectItem::Type::Container);
        ASSERT_NE(container, nullptr);
        for (const auto& name : {QStringLiteral("First"), QStringLiteral("Second")})
            ASSERT_NE(manager.createItem(container, name, ProjectItem::Type::Sound), nullptr);
        ASSERT_NE(manager.createItem(segment, QStringLiteral("Timeline"),
                                     ProjectItem::Type::Track), nullptr);
        for (const auto& [name, type] : {
                 std::pair{QStringLiteral("Trigger"), ProjectItem::Type::Action},
                 std::pair{QStringLiteral("Space"), ProjectItem::Type::Attenuation},
                 std::pair{QStringLiteral("Intensity"), ProjectItem::Type::Variable},
             }) {
            auto* item = manager.createItem(nullptr, name, type);
            ASSERT_NE(item, nullptr);
            if (type == ProjectItem::Type::Variable) variableGuid_ = item->guid;
        }
        if (includeEffectDocuments_)
            ASSERT_NE(manager.createItem(nullptr, QStringLiteral("Limiter"),
                                         ProjectItem::Type::Effect), nullptr);
        auto* bus = manager.createItem(manager.getMasterBusItem(),
                                       QStringLiteral("Music"), ProjectItem::Type::Bus);
        ASSERT_NE(bus, nullptr);
        bus->comp<VBus>()->pan = 0.25f;
        bus->comp<VBus>()->channelConfig = 2;
        bus->comp<VBus>()->listenerRelativeRouting = true;
        ASSERT_TRUE(manager.saveProject());
        projectPath_ = manager.currentProject()->projectPath;
        projectDirectory_ = QFileInfo(projectPath_).absolutePath();
        ASSERT_TRUE(manager.closeProject());

        QDirIterator documents(projectDirectory_,
                                {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                                QDir::Files, QDirIterator::Subdirectories);
        while (documents.hasNext()) {
            const QString path = documents.next();
            QJsonObject document = readObject(path);
            QJsonObject header = HarkenTest::projectHeader(document);
            const QString id = header.value(QStringLiteral("id")).toString();
            const QJsonObject metadata = extensionMetadata(id);
            header.insert(QStringLiteral("futureHeaderMetadata"), metadata);
            document.insert(QStringLiteral("harkenDocument"), header);
            writeObject(path, document);
            documentsById_.insert(id, document);
            pathsById_.insert(id, path);
        }
        ASSERT_FALSE(documentsById_.isEmpty());
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    void expectModelMetadata(ProjectData& project)
    {
        for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
            const QJsonObject expectedHeader = HarkenTest::projectHeader(it.value());
            QJsonObject header;
            if (expectedHeader.value(QStringLiteral("type"))
                == QStringLiteral("project.root")) {
                header = project.documentHeader;
            } else {
                ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
                ASSERT_NE(item, nullptr) << it.key().toStdString();
                header = item->documentHeader;
                if (const auto* bus = item->comp<VBus>())
                    EXPECT_EQ(bus->toJson(), HarkenTest::projectContent(it.value())
                        .value(QStringLiteral("components")).toObject().value(QStringLiteral("bus")).toObject());
            }
            EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                      expectedHeader.value(QStringLiteral("futureHeaderMetadata")));
        }
    }

    QMap<QString, QByteArray> savedDocumentContent()
    {
        QMap<QString, QByteArray> result;
        const auto paths = documentPathsById(projectDirectory_);
        for (auto it = paths.cbegin(); it != paths.cend(); ++it)
            result.insert(it.value(), readFile(it.value()));
        return result;
    }

    QTemporaryDir directory_;
    QString projectPath_;
    QString projectDirectory_;
    QUuid segmentGuid_;
    QUuid variableGuid_;
    QMap<QString, QJsonObject> documentsById_;
    QMap<QString, QString> pathsById_;
    bool includeEffectDocuments_ = true;
};

TEST_F(ProjectDocumentRoundTripTest, AllDocumentMetadataSurvivesEditingSavingAndReopening)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    actions.clearAllForTesting();
    const auto cleanup = qScopeGuard([&] {
        UndoRedoManager::instance().clearHistory();
        actions.clearAllForTesting();
    });
    registerProjectPlatformActions(actions, &manager);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    expectModelMetadata(project);
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it)
        EXPECT_EQ(readObject(it.value()), documentsById_.value(it.key()));

    project.name = QStringLiteral("Edited by the baseline reader");
    project.sampleRate = 44100;
    project.channelConfig = AudioChannelConfig::Surround_5_1;
    QJsonArray editedPlatforms;
    for (const auto& platform : project.platforms) {
        if (platform.basePlatform == BasePlatform::Windows || platform.basePlatform == BasePlatform::Android)
            editedPlatforms.append(platform.toJson());
    }
    ASSERT_EQ(editedPlatforms.size(), 2);
    const auto platformEdit = actions.execute(QStringLiteral("apply_project_platforms"), {
        {QStringLiteral("expected_platforms"), ProjectPlatformSemantics::platformsToJson(project.platforms)},
        {QStringLiteral("platforms"), editedPlatforms}});
    ASSERT_TRUE(platformEdit.success) << platformEdit.errorMessage.toStdString();
    ASSERT_TRUE(manager.addProjectLanguage(QStringLiteral("English")));
    ASSERT_TRUE(manager.addProjectLanguage(QStringLiteral("Japanese")));
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        const QJsonObject header = HarkenTest::projectHeader(it.value());
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.root"))
            continue;
        ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
        ASSERT_NE(item, nullptr);
        if (item == manager.getMasterBusItem()) continue;
        const QString name = QStringLiteral("Edited_") + item->name;
        ASSERT_TRUE(manager.renameItem(item, name));
    }
    ASSERT_TRUE(manager.saveProject());
    expectModelMetadata(project);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& reopened = *manager.currentProject();
    expectModelMetadata(reopened);
    EXPECT_EQ(reopened.sampleRate, 44100);
    EXPECT_EQ(reopened.channelConfig, AudioChannelConfig::Surround_5_1);
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(reopened.platforms),
              editedPlatforms);
    EXPECT_EQ(reopened.projectLanguages, (QStringList{"English", "Japanese"}));
    const auto savedPaths = documentPathsById(projectDirectory_);
    EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
    for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
        const QJsonObject document = readObject(it.value());
        const QJsonObject header = HarkenTest::projectHeader(document);
        EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                  HarkenTest::projectHeader(documentsById_.value(it.key()))
                      .value(QStringLiteral("futureHeaderMetadata")));
    }
    ASSERT_TRUE(manager.removeProjectLanguage(QStringLiteral("English")));
    ASSERT_TRUE(manager.removeProjectLanguage(QStringLiteral("Japanese")));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    EXPECT_TRUE(manager.currentProject()->projectLanguages.isEmpty());
}

class ProjectDocumentVersionRoundTripTest : public ProjectDocumentRoundTripTest,
                                          public ::testing::WithParamInterface<int>
{
protected:
    void SetUp() override
    {
        includeEffectDocuments_ = GetParam() >= ProjectSchema::kProjectSchemaVersion;
        ProjectDocumentRoundTripTest::SetUp();
        if (GetParam() >= ProjectSchema::kProjectSchemaVersion) return;
        for (const auto& id : documentsById_.keys()) {
            if (HarkenTest::projectHeader(documentsById_.value(id)).value(QStringLiteral("type"))
                != QStringLiteral("project.conversion")) continue;
            ASSERT_TRUE(QFile::remove(pathsById_.value(id)));
            pathsById_.remove(id);
            documentsById_.remove(id);
        }

    }
};

TEST_P(ProjectDocumentVersionRoundTripTest, SupportedDocumentVersionsPreserveContentThroughOpenSave)
{
    auto& manager = ProjectManager::instance();
    const int version = GetParam();
    const bool historical = version < ProjectSchema::kProjectSchemaVersion;
    const bool userDataBlocked = version == 7;
    SCOPED_TRACE(version);
    QMap<QString, QByteArray> beforeOpen;
    QMap<QString, QJsonObject> expectedProperties;
    QMap<QString, QJsonObject> expectedGroups;
    QMap<QString, QJsonArray> expectedBusEffects;
    const auto platforms = HarkenTest::projectContent(readObject(projectPath_))
        .value(QStringLiteral("deployPlatforms")).toArray();
    ASSERT_FALSE(platforms.isEmpty());
    const auto platformId = platforms.first().toObject().value(QStringLiteral("id"));
    ASSERT_TRUE(platformId.isString());
    const QDir projectRoot(projectDirectory_);
    const auto layoutPath = projectRoot.filePath(QStringLiteral(".cache/editor_layout.json"));
    const auto userLayoutPath = projectRoot.filePath(QStringLiteral(".user/editor_layout.json"));
    const QJsonObject layout{{QStringLiteral("baseLayout"), QStringLiteral("Mixing")}};
    const QJsonArray labelCell{QJsonObject{{QStringLiteral("labelId"), QUuid::createUuidV5(variableGuid_,
        QByteArray("grid-label-1")).toString(QUuid::WithoutBraces)}}};
    QJsonArray expectedRows;
    QString containerId;
    if (historical) {
        ASSERT_TRUE(projectRoot.mkpath(QStringLiteral(".cache/asset_registry")));
        writeObject(layoutPath, layout);
        writeObject(projectRoot.filePath(QStringLiteral(".cache/asset_registry/assets.json")), QJsonObject{});
        if (userDataBlocked) writeObject(projectRoot.filePath(QStringLiteral(".user")), QJsonObject{});
    }
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        auto document = it.value();
        auto header = HarkenTest::projectHeader(document);
        header.insert(QStringLiteral("schemaVersion"), version);
        if (version < ProjectSchema::kProjectSchemaVersion) {
            header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.0.0"));
            header.insert(QStringLiteral("propertyModel"), QStringLiteral("fieldGroupsV1"));
            header.remove(QStringLiteral("studioVersion"));
        }
        document.insert(QStringLiteral("harkenDocument"), header);
        auto content = HarkenTest::projectContent(document);
        if (version < ProjectSchema::kProjectSchemaVersion) {
            removeConversionGroup(content);
            if (header.value(QStringLiteral("type")) == QStringLiteral("project.root")) {
                auto audio = content.value(QStringLiteral("audio")).toObject();
                audio.remove(QStringLiteral("defaultConversions"));
                content.insert(QStringLiteral("audio"), audio);
                auto legacyPlatforms = content.value(QStringLiteral("deployPlatforms")).toArray();
                for (qsizetype index = 0; index < legacyPlatforms.size(); ++index) {
                    auto platform = legacyPlatforms[index].toObject();
                    QJsonArray tiers;
                    for (const auto& [limit, rate] : {std::pair{100, 4000}, {300, 32000}, {1000, 30000}, {0, 96000}})
                        tiers.append(QJsonObject{
                            {QStringLiteral("maxBytes"), limit > 0 ? QJsonValue(limit) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("config"), legacyConversionParameters(rate)}});
                    platform.insert(QStringLiteral("profile"), legacyProfile(index == 0, tiers));
                    legacyPlatforms[index] = platform;
                }
                content.insert(QStringLiteral("deployPlatforms"), legacyPlatforms);
            }
        }
        auto authoredComponents = content.value(QStringLiteral("components")).toObject();
        if (content.value(QStringLiteral("playableProperties")).isObject()) {
            auto properties = content.value(QStringLiteral("playableProperties")).toObject();
            ASSERT_TRUE(properties.value(QStringLiteral("platformOverrides")).toArray().isEmpty());
            QJsonObject overlay{{QStringLiteral("platformId"), platformId},
                {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
                {QStringLiteral("volume"), -3.0}};
            properties.insert(QStringLiteral("platformOverrides"), QJsonArray{overlay});
            expectedProperties.insert(it.key(), properties);
            if (historical) {
                bool validPresence = false;
                const auto presence = properties.value(QStringLiteral("presence")).toString()
                    .toULongLong(&validPresence, 16);
                ASSERT_TRUE(validPresence);
                properties.insert(QStringLiteral("presence"),
                    QStringLiteral("%1").arg(presence | 0x9004, 16, 16, QLatin1Char('0')));
                properties.insert(QStringLiteral("makeUpGain"), 6.0);
                properties.insert(QStringLiteral("signalGraph"), QJsonArray{});
                properties.insert(QStringLiteral("spatial.attenuationCurveType"), 1);
                overlay.insert(QStringLiteral("presence"), QStringLiteral("0000000000000005"));
                overlay.insert(QStringLiteral("makeUpGain"), -12.0);
                auto mix = authoredComponents.value(QStringLiteral("mixnode")).toObject();
                mix.insert(QStringLiteral("makeUpGain"), 9.0);
                authoredComponents.insert(QStringLiteral("mixnode"), mix);
            }
            properties.insert(QStringLiteral("platformOverrides"), QJsonArray{overlay});
            content.insert(QStringLiteral("playableProperties"), properties);
            content.insert(QStringLiteral("components"), authoredComponents);
        }
        if (version < ProjectSchema::kProjectSchemaVersion) {
            QJsonArray effects;
            QJsonArray legacyEffects;
            const bool bus = header.value(QStringLiteral("type")) == QStringLiteral("project.bus");
            if (bus || it.key() == segmentGuid_.toString(QUuid::WithoutBraces)) {
                EffectEntry effect;
                effect.guid = QUuid::createUuid();
                effect.mode = EffectMode::Custom;
                effect.custom.type = EffectType::Limiter;
                effect.custom.parameters = {-4.5f, 80.0f};
                effect.bypass = true;
                effect.extensions = extensionMetadata(effect.guid.toString(QUuid::WithoutBraces));
                effects.append(effect.toJson());
                auto legacyEffect = effect.extensions;
                legacyEffect.insert(QStringLiteral("guid"), effect.guid.toString(QUuid::WithoutBraces));
                legacyEffect.insert(QStringLiteral("type"), QStringLiteral("limiter"));
                legacyEffect.insert(QStringLiteral("parameters"), QJsonArray{-4.5, 80.0});
                legacyEffect.insert(QStringLiteral("bypass"), true);
                legacyEffects.append(legacyEffect);
                if (bus) expectedBusEffects.insert(it.key(), effects);
            }
            if (bus || content.value(QStringLiteral("fieldGroups")).isObject()) {
                auto components = content.value(QStringLiteral("components")).toObject();
                auto mix = components.value(QStringLiteral("mixnode")).toObject();
                mix.insert(QStringLiteral("effects"), legacyEffects);
                components.insert(QStringLiteral("mixnode"), mix);
                content.insert(QStringLiteral("components"), components);
            }
            if (content.value(QStringLiteral("fieldGroups")).isObject()) {
                auto groups = content.value(QStringLiteral("fieldGroups")).toObject();
                auto shared = groups.value(QStringLiteral("shared")).toObject();
                shared.remove(QStringLiteral("conversion"));
                shared.insert(QStringLiteral("effect"), QJsonObject{
                    {QStringLiteral("inherit"), false},
                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("effects"), effects}}}});
                groups.insert(QStringLiteral("shared"), shared);
                QJsonObject outputValues{{QStringLiteral("outputBusVolume"), -2.0}};
                const auto outputReference = shared.value(QStringLiteral("output")).toObject()
                    .value(QStringLiteral("values")).toObject().value(QStringLiteral("outputBusRef"));
                if (!outputReference.isUndefined())
                    outputValues.insert(QStringLiteral("outputBusRef"), outputReference);
                QJsonObject platformScope{{QStringLiteral("output"), QJsonObject{
                    {QStringLiteral("values"), outputValues}}}};
                groups.insert(QStringLiteral("platforms"), QJsonObject{
                    {platformId.toString(), platformScope}});
                expectedGroups.insert(it.key(), groups);
                const auto restoreOutput = [](QJsonObject* scope) {
                    auto output = scope->take(QStringLiteral("output")).toObject();
                    if (output.value(QStringLiteral("values")).isObject()) {
                        auto values = output.value(QStringLiteral("values")).toObject();
                        if (values.contains(QStringLiteral("outputBusRef")))
                            values.insert(QStringLiteral("mainOutputBusRef"),
                                          values.take(QStringLiteral("outputBusRef")));
                        output.insert(QStringLiteral("values"), values);
                    }
                    scope->insert(QStringLiteral("mainOutput"), output);
                };
                restoreOutput(&shared);
                shared.remove(QStringLiteral("effect"));
                groups.insert(QStringLiteral("shared"), shared);
                restoreOutput(&platformScope);
                groups.insert(QStringLiteral("platforms"), QJsonObject{
                    {platformId.toString(), platformScope}});
                content.insert(QStringLiteral("fieldGroups"), groups);
            }
            if (header.value(QStringLiteral("type")) == QStringLiteral("project.root")) {
                content.insert(QStringLiteral("paths"), QJsonObject{
                    {QStringLiteral("audioFolder"), QStringLiteral("Layers")},
                });
                auto metadata = content.value(QStringLiteral("metadata")).toObject();
                const auto audio = content.take(QStringLiteral("audio")).toObject();
                metadata.insert(QStringLiteral("sampleRate"), audio.value(QStringLiteral("renderSampleRate")));
                metadata.insert(QStringLiteral("channelConfig"), audio.value(QStringLiteral("channelConfig")));
                metadata.insert(QStringLiteral("platforms"), content.take(QStringLiteral("deployPlatforms")));
                content.insert(QStringLiteral("projectLanguages"), content.take(QStringLiteral("deployLanguages")));
                metadata.insert(QStringLiteral("createdDate"), QStringLiteral("2001-02-03T04:05:06Z"));
                metadata.insert(QStringLiteral("modifiedDate"), QStringLiteral("2001-02-03T04:05:06Z"));
                metadata.insert(QStringLiteral("toolVersion"), QStringLiteral("HarkenStudio 1.0.0"));
                content.insert(QStringLiteral("metadata"), metadata);
            } else if (header.value(QStringLiteral("type")) == QStringLiteral("project.bus")) {
                content.insert(QStringLiteral("componentData"),
                               content.value(QStringLiteral("components")).toObject().value(QStringLiteral("bus")));
                auto properties = content.value(QStringLiteral("playableProperties")).toObject();
                if (properties.contains(QStringLiteral("outputBusRef")))
                    properties.insert(QStringLiteral("mainOutputBusRef"),
                                      properties.take(QStringLiteral("outputBusRef")));
                auto overlays = properties.value(QStringLiteral("platformOverrides")).toArray();
                for (qsizetype index = 0; index < overlays.size(); ++index) {
                    auto overlay = overlays[index].toObject();
                    if (overlay.contains(QStringLiteral("outputBusRef")))
                        overlay.insert(QStringLiteral("mainOutputBusRef"),
                                       overlay.take(QStringLiteral("outputBusRef")));
                    overlays[index] = overlay;
                }
                properties.insert(QStringLiteral("platformOverrides"), overlays);
                content.insert(QStringLiteral("playableProperties"), properties);
            } else if (header.value(QStringLiteral("type")) != QStringLiteral("project.action")) {
                content.insert(QStringLiteral("expanded"), true);
            }
        }
        if (version == 7 && header.value(QStringLiteral("type")) == QStringLiteral("project.root")) {
            const auto platform = content.value(QStringLiteral("metadata")).toObject()
                .value(QStringLiteral("platforms")).toArray().first().toObject()
                .value(QStringLiteral("id"));
            content.insert(QStringLiteral("packExportCurrentPlatformId"), platform);
            content.insert(QStringLiteral("packExportSelectedPlatformIds"), QJsonArray{platform});
        }
        if (historical && content.value(QStringLiteral("type")) == QStringLiteral("Container")) {
            containerId = it.key();
            auto components = content.value(QStringLiteral("components")).toObject();
            auto selection = components.value(QStringLiteral("selection")).toObject();
            auto selections = selection.value(QStringLiteral("selections")).toObject();
            const auto condition = [](int ordinal, int priority, const QJsonArray& cells) {
                return QJsonObject{{QStringLiteral("stableRuleOrdinal"), ordinal},
                    {QStringLiteral("priority"), priority}, {QStringLiteral("cells"), cells}};
            };
            auto table = selections.value(QStringLiteral("shared")).toObject();
            table.remove(QStringLiteral("matchRows"));
            table.insert(QStringLiteral("matchAxes"), QJsonArray{variableGuid_.toString(QUuid::WithoutBraces)});
            table.insert(QStringLiteral("ruleOrdinalHighWater"), 3);
            auto entries = table.value(QStringLiteral("entries")).toArray();
            ASSERT_EQ(entries.size(), 2);
            auto first = entries[0].toObject();
            auto second = entries[1].toObject();
            first.insert(QStringLiteral("conditions"), QJsonArray{
                condition(0, 2, {QJsonValue::Null}), condition(1, 0, {1})});
            second.insert(QStringLiteral("conditions"), QJsonArray{condition(2, 1, {1})});
            const auto expectRow = [&](int ordinal, const QJsonObject& entry, const QJsonArray& cells) {
                expectedRows.append(QJsonObject{{QStringLiteral("stableRuleOrdinal"), ordinal},
                    {QStringLiteral("target"), entry.value(QStringLiteral("childGuid"))},
                    {QStringLiteral("cells"), cells}});
            };
            expectRow(1, first, labelCell);
            expectRow(2, second, labelCell);
            expectRow(0, first, {QJsonValue::Null});
            table.insert(QStringLiteral("entries"), QJsonArray{first, second});
            selections.insert(QStringLiteral("shared"), table);
            selections.insert(QStringLiteral("platforms"), QJsonObject{{platformId.toString(), table}});
            selection.insert(QStringLiteral("selections"), selections);
            components.insert(QStringLiteral("selection"), selection);
            content.insert(QStringLiteral("components"), components);
        }
        if (historical && content.value(QStringLiteral("type")) == QStringLiteral("Segment")) {
            content.insert(QStringLiteral("type"), QStringLiteral("Piece"));
            auto components = content.value(QStringLiteral("components")).toObject();
            auto timeline = components.take(QStringLiteral("segmenttimeline")).toObject();
            timeline.insert(QStringLiteral("pieceTime"),
                            QJsonObject{{QStringLiteral("domain"), QStringLiteral("Absolute")}});
            components.insert(QStringLiteral("piecetimeline"), timeline);
            content.insert(QStringLiteral("components"), components);
        }
        document.insert(QStringLiteral("content"), content);
        const QString path = pathsById_.value(it.key());
        writeObject(path, document);
        beforeOpen.insert(path, readFile(path));
    }
    if (version < ProjectSchema::kProjectSchemaVersion) {
        const auto errors = ProjectContractRegistry::instance().validateTree(
            projectDirectory_, readObject(projectPath_), false);
        ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    }
    ProjectUpgradeManager upgrades;
    const auto analysis = upgrades.analyzeForProjectOpen(projectPath_);
    ASSERT_TRUE(analysis.ok) << analysis.blockingErrors.join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(analysis.upgradeRequired, version < ProjectSchema::kProjectSchemaVersion);
    for (auto it = beforeOpen.cbegin(); it != beforeOpen.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value());
    if (historical) {
        auto trial = upgrades.execute(analysis);
        ASSERT_TRUE(trial.ok) << trial.error.toStdString();
        QString rollbackError;
        ASSERT_TRUE(upgrades.rollback(trial, &rollbackError)) << rollbackError.toStdString();
        EXPECT_EQ(savedDocumentContent(), beforeOpen);
        EXPECT_EQ(readObject(layoutPath), layout);
        for (const auto& path : analysis.migrationPlan.writeSet) {
            const auto absolute = projectRoot.filePath(path);
            if (!beforeOpen.contains(absolute)) EXPECT_FALSE(QFileInfo::exists(absolute));
        }
    }
    if (analysis.upgradeRequired) {
        const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto digest = projectOpenApprovalDigest(analysis, requestId);
        auto prepared = ProjectManager::prepareProject(projectPath_, true, {},
            [&](const ProjectUpgradeAnalysis& current) {
                return current.ok && current.upgradeRequired
                    && projectOpenApprovalDigest(current, requestId) == digest;
            });
        ASSERT_TRUE(prepared.project) << prepared.error.toStdString();
        ASSERT_TRUE(manager.commitPreparedProject(std::move(prepared)));
    } else {
        QString error;
        ASSERT_TRUE(manager.loadProject(projectPath_, false, &error)) << error.toStdString();
    }
    UndoRedoManager::instance().clearHistory();
    expectModelMetadata(*manager.currentProject());
    if (historical) {
        EXPECT_EQ(QFileInfo::exists(layoutPath), userDataBlocked);
        EXPECT_EQ(QFileInfo::exists(userLayoutPath), !userDataBlocked);
        if (!userDataBlocked) {
            auto imported = readObject(userLayoutPath);
            imported.remove(QStringLiteral("version"));
            EXPECT_EQ(imported, layout);
        }
        const auto* container = manager.findItemByGuid(QUuid(containerId));
        ASSERT_NE(container, nullptr);
        for (const auto* table : container->comp<VSelection>()->selections.tables())
            EXPECT_EQ(table->toJson().value(QStringLiteral("matchRows")).toArray(), expectedRows);
        const auto& defaults = manager.currentProject()->defaultConversionRefs;
        EXPECT_EQ(defaults.size(), 1);
        std::vector<std::pair<qint64, quint32>> rules;
        for (auto* binding : manager.currentProject()->allFileBindings())
            if (const auto* conversion = binding->rootItem ? binding->rootItem->comp<VConversion>() : nullptr)
                for (const auto& rule : conversion->config.rules)
                    rules.push_back({rule.maxBytes ? static_cast<qint64>(*rule.maxBytes) : -1, rule.sampleRate});
        EXPECT_EQ(rules, (std::vector<std::pair<qint64, quint32>>{
            {100, 8000}, {300, 32000}, {1000, 24000}, {-1, 0}}));
    }
    auto* segment = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment->type, ProjectItem::Type::Segment);
    EXPECT_EQ(segment->name, QStringLiteral("Piece"));
    ASSERT_NE(segment->comp<VSegment>(), nullptr);
    if (!analysis.upgradeRequired)
        for (auto it = beforeOpen.cbegin(); it != beforeOpen.cend(); ++it)
            EXPECT_EQ(readFile(it.key()), it.value());
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    const auto allSavedPaths = documentPathsById(projectDirectory_);
    auto savedPaths = allSavedPaths;
    if (analysis.upgradeRequired) {
        for (const auto& id : savedPaths.keys())
            if (HarkenTest::projectHeader(readObject(savedPaths.value(id)))
                    .value(QStringLiteral("type")) == QStringLiteral("project.conversion"))
                savedPaths.remove(id);
    }
    EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
    if (!historical)
        EXPECT_EQ(allSavedPaths.size(), documentsById_.size());
    else
        EXPECT_GT(allSavedPaths.size(), documentsById_.size());
    for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
        const auto document = readObject(it.value());
        EXPECT_EQ(HarkenTest::projectHeader(document).value(QStringLiteral("schemaVersion")),
                  ProjectSchema::kProjectSchemaVersion);
        const auto content = HarkenTest::projectContent(document);
        const auto original = HarkenTest::projectContent(documentsById_.value(it.key()));
        if (expectedGroups.contains(it.key())) {
            auto stripped = content;
            removeConversionGroup(stripped);
            EXPECT_EQ(stripped.value(QStringLiteral("fieldGroups")).toObject(), expectedGroups.value(it.key()));
        }
        if (expectedProperties.contains(it.key())) {
            EXPECT_EQ(content.value(QStringLiteral("playableProperties")),
                      expectedProperties.value(it.key()));
            auto expectedMix = original.value(QStringLiteral("components")).toObject()
                .value(QStringLiteral("mixnode"));
            if (expectedBusEffects.contains(it.key())) {
                auto mix = expectedMix.toObject();
                mix.insert(QStringLiteral("effects"), expectedBusEffects.value(it.key()));
                expectedMix = mix;
            }
            EXPECT_EQ(content.value(QStringLiteral("components")).toObject()
                          .value(QStringLiteral("mixnode")),
                      expectedMix);
        }
        for (const auto* key : {"type", "name", "guid", "parentRef"})
            EXPECT_EQ(content.value(QLatin1String(key)), original.value(QLatin1String(key)));
        if (HarkenTest::projectHeader(document).value(QStringLiteral("type")) == QStringLiteral("project.root")) {
            for (const auto* key : {"metadata", "deployPlatforms", "deployLanguages"})
                EXPECT_EQ(content.value(QLatin1String(key)), original.value(QLatin1String(key)));
            auto actualAudio = content.value(QStringLiteral("audio")).toObject();
            auto expectedAudio = original.value(QStringLiteral("audio")).toObject();
            for (const auto& reference : actualAudio.value(QStringLiteral("defaultConversions")).toObject())
                EXPECT_TRUE(allSavedPaths.contains(reference.toObject().value(QStringLiteral("id")).toString()));
            if (analysis.upgradeRequired) {
                actualAudio.remove(QStringLiteral("defaultConversions"));
                expectedAudio.remove(QStringLiteral("defaultConversions"));
            }
            EXPECT_EQ(actualAudio, expectedAudio);
        }
        if (it.key() == segmentGuid_.toString(QUuid::WithoutBraces))
            EXPECT_EQ(content.value(QStringLiteral("components")),
                      original.value(QStringLiteral("components")));
    }
    const auto normalized = savedDocumentContent();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    UndoRedoManager::instance().clearHistory();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), normalized);
    ASSERT_TRUE(manager.closeProject());
}

INSTANTIATE_TEST_SUITE_P(Current, ProjectDocumentVersionRoundTripTest,
                         ::testing::Values(ProjectSchema::kProjectSchemaVersion));

INSTANTIATE_TEST_SUITE_P(Historical, ProjectDocumentVersionRoundTripTest,
                         ::testing::Values(7, 8, 9));

TEST_F(ProjectDocumentRoundTripTest, RenamingAnAncestorDoesNotRewriteDescendantDocuments)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    UndoRedoManager::instance().clearHistory();
    auto* segment = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(segment, nullptr);
    auto* ancestor = segment->parent;
    ASSERT_NE(ancestor, nullptr);
    const auto ancestorId = ancestor->guid.toString(QUuid::WithoutBraces);
    const auto committed = savedDocumentContent();
    QMap<QString, QDateTime> modificationTimes;
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it)
        modificationTimes.insert(it.key(), QFileInfo(it.value()).lastModified());
    ASSERT_TRUE(manager.renameItem(ancestor, QStringLiteral("RenamedMain")));
    EXPECT_FALSE(segment->isDirtyObject());
    ASSERT_TRUE(manager.saveProject());
    const auto renamedPaths = documentPathsById(projectDirectory_);
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it) {
        const auto path = renamedPaths.value(it.key());
        if (it.key() == ancestorId) {
            EXPECT_NE(readFile(path), committed.value(it.value()));
            continue;
        }
        EXPECT_EQ(readFile(path), committed.value(it.value())) << path.toStdString();
        EXPECT_EQ(QFileInfo(path).lastModified(), modificationTimes.value(it.key())) << path.toStdString();
    }
    const auto renamed = savedDocumentContent();
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    segment = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(segment, nullptr);
    ASSERT_NE(segment->parent, nullptr);
    EXPECT_EQ(segment->parent->name, QStringLiteral("RenamedMain"));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), renamed);
}

TEST_F(ProjectDocumentRoundTripTest, UnchangedDocumentsPreserveBytesAndModificationTimesAcrossSaves)
{
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());

    const auto savedTime = QDateTime::fromString(QStringLiteral("2001-02-03T04:05:06Z"), Qt::ISODate);

    QMap<QString, QByteArray> committed;
    QMap<QString, QDateTime> modificationTimes;
    const auto paths = documentPathsById(projectDirectory_);
    for (const auto& path : paths) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite)) << path.toStdString();
        ASSERT_TRUE(file.setFileTime(savedTime, QFileDevice::FileModificationTime))
            << path.toStdString();
        file.close();
        committed.insert(path, readFile(path));
        modificationTimes.insert(path, QFileInfo(path).lastModified());
    }

    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), modificationTimes.value(it.key()))
            << it.key().toStdString();
    }

    ASSERT_TRUE(manager.closeProject());
    auto document = readObject(projectPath_);
    const auto currentHeader = HarkenTest::projectHeader(document);
    auto previousHeader = currentHeader;
    previousHeader.insert(QStringLiteral("studioVersion"), QStringLiteral("0.0.0"));
    previousHeader.insert(QStringLiteral("packSchemaVersion"), HARKEN_PACK_METADATA_VERSION + 1);
    document.insert(QStringLiteral("harkenDocument"), previousHeader);
    writeObject(projectPath_, document);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    EXPECT_EQ(HarkenTest::projectHeader(readObject(projectPath_)), previousHeader);
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(HarkenTest::projectHeader(readObject(projectPath_)), currentHeader);
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        if (it.key() == projectPath_) continue;
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), modificationTimes.value(it.key()))
            << it.key().toStdString();
    }

    const QString name = QStringLiteral("Edited after an unchanged save");
    manager.currentProject()->name = name;
    ASSERT_TRUE(manager.saveProject());
    const auto savedMetadata = HarkenTest::projectContent(readObject(projectPath_))
        .value(QStringLiteral("metadata")).toObject();
    EXPECT_EQ(savedMetadata.value(QStringLiteral("name")).toString(), name);
    EXPECT_GT(QFileInfo(projectPath_).lastModified(), modificationTimes.value(projectPath_));
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        if (it.key() == projectPath_) continue;
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), modificationTimes.value(it.key()))
            << it.key().toStdString();
    }
}

TEST_F(ProjectDocumentRoundTripTest, OrdinaryEditsAndUndoPreserveTheSameDocumentsAcrossSavePaths)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    actions.clearAllForTesting();
    const auto cleanup = qScopeGuard([&] {
        undo.clearHistory();
        actions.clearAllForTesting();
    });
    registerProjectActions(actions, &manager, nullptr);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    const auto original = savedDocumentContent();
    QList<QUuid> edited;
    qsizetype editCount = 0;
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        if (HarkenTest::projectHeader(it.value()).value(QStringLiteral("type"))
            == QStringLiteral("project.root")) continue;
        auto* item = manager.findItemByGuid(QUuid(it.key()));
        ASSERT_NE(item, nullptr);
        SCOPED_TRACE(item->name.toStdString());
        QList<std::pair<QString, QJsonValue>> properties{
            {QStringLiteral("notes"), QStringLiteral("Edited after opening\n") + item->name}};
        if (item->type == ProjectItem::Type::Bus) {
            properties.append({QStringLiteral("volume"), -6.0});
            if (item != manager.getMasterBusItem())
                properties.append({QStringLiteral("sends"), QJsonArray{QJsonObject{
                    {QStringLiteral("targetBusID"), static_cast<qint64>(manager.getMasterBusItem()->objectID)},
                    {QStringLiteral("type"), QStringLiteral("Sidechain")},
                    {QStringLiteral("duckTarget"), false},
                    {QStringLiteral("sendLevel"), -6.0}}}});
        } else if (item->type == ProjectItem::Type::Action) {
            const auto* target = manager.findItemByGuid(segmentGuid_);
            ASSERT_NE(target, nullptr);
            VActionStep step;
            step.targetObjectID = target->objectID;
            step.targetName = target->name;
            properties.append({QStringLiteral("steps"), ActionStepEditing::stepsToJson({step})});
        }
        for (const auto& [property, value] : properties) {
            const auto result = actions.execute(QStringLiteral("set_item_property"), {
                {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
                {QStringLiteral("property"), property}, {QStringLiteral("value"), value}});
            ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
            ++editCount;
        }
        EXPECT_TRUE(item->dirtyBit);
        edited.append(item->guid);
    }
    ASSERT_FALSE(edited.isEmpty());
    for (qsizetype i = 0; i < editCount; ++i) ASSERT_TRUE(undo.undo());
    for (const auto& guid : edited) EXPECT_FALSE(manager.findItemByGuid(guid)->dirtyBit);
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), original);

    for (qsizetype i = 0; i < editCount; ++i) ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(manager.saveProject());
    const auto saved = savedDocumentContent();
    const auto authoredItems = [&] {
        ProjectSerializer serializer;
        EXPECT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
        QMap<QUuid, QJsonObject> states;
        for (const auto& guid : edited) {
            const auto* item = manager.findItemByGuid(guid);
            EXPECT_NE(item, nullptr);
            if (item) {
                auto state = serializer.projectItemToJson(*item, false);
                if (const auto* mix = item->comp<VMixNode>())
                    state.insert(QStringLiteral("authoredSends"),
                                 QJsonValue::fromVariant(mix->getField(QStringLiteral("sends"))));
                states.insert(guid, state);
            }
        }
        return states;
    };
    const auto authored = authoredItems();
    EXPECT_NE(saved, original);
    EXPECT_EQ(saved.value(projectPath_), original.value(projectPath_));
    for (const auto& guid : edited) EXPECT_FALSE(manager.findItemByGuid(guid)->dirtyBit);
    undo.clear();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), saved);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    expectModelMetadata(*manager.currentProject());
    EXPECT_EQ(authoredItems(), authored);
    for (const auto& guid : edited) {
        const auto* item = manager.findItemByGuid(guid);
        ASSERT_NE(item, nullptr);
        EXPECT_FALSE(item->dirtyBit);
    }
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), saved);
}

#ifdef Q_OS_WIN
TEST_F(ProjectDocumentRoundTripTest, FailedObjectSaveRetainsEditsForRetryAndSubsequentUndo)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    actions.clearAllForTesting();
    const auto cleanup = qScopeGuard([&] {
        undo.clearHistory();
        actions.clearAllForTesting();
    });
    registerProjectActions(actions, &manager, nullptr);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    const auto committed = savedDocumentContent();
    const auto committedRoot = readFile(projectPath_);
    auto* item = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(item, nullptr);
    const QString occupiedPath = manager.currentProject()->objectFilePath(item);
    ASSERT_TRUE(committed.contains(occupiedPath));
    HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(occupiedPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] {
        if (occupied != INVALID_HANDLE_VALUE) CloseHandle(occupied);
    });
    const auto result = actions.execute(QStringLiteral("set_item_property"), {
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), QStringLiteral("notes")},
        {QStringLiteral("value"), QStringLiteral("Retained after failed save")}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_FALSE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), committed);
    EXPECT_EQ(readFile(projectPath_), committedRoot);
    EXPECT_TRUE(item->dirtyBit);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(CloseHandle(occupied));
    occupied = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(item->dirtyBit);
    EXPECT_TRUE(undo.isClean());
    EXPECT_NE(savedDocumentContent(), committed);
    ASSERT_TRUE(undo.undo());
    EXPECT_TRUE(item->dirtyBit);
    ASSERT_TRUE(manager.saveProject());
    const auto restored = savedDocumentContent();
    EXPECT_EQ(restored.keys(), committed.keys());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        if (it.key() == occupiedPath)
            EXPECT_EQ(QJsonDocument::fromJson(restored.value(it.key())),
                      QJsonDocument::fromJson(it.value()));
        else
            EXPECT_EQ(restored.value(it.key()), it.value()) << it.key().toStdString();
    }
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    item = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->notes.isEmpty());
    expectModelMetadata(*manager.currentProject());
}

TEST_F(ProjectDocumentRoundTripTest, OccupiedFilesDoNotDamageCommittedContentWhenSavingFails)
{
    const QDir root(projectDirectory_);
    const QString folderPath = root.filePath(QStringLiteral("Layers/Occupied"));
    const QString emptyPath = QDir(folderPath).filePath(QStringLiteral("Empty"));
    const QString ordinaryPath = QDir(folderPath).filePath(QStringLiteral("000_locked.bin"));
    ASSERT_TRUE(QDir().mkpath(emptyPath));
    QFile ordinary(ordinaryPath);
    ASSERT_TRUE(ordinary.open(QIODevice::WriteOnly));
    ASSERT_EQ(ordinary.write("retained bytes"), 14);
    ordinary.close();

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    auto* sound = manager.createItem(nullptr, QStringLiteral("OccupiedSound"),
        ProjectItem::Type::Sound, QStringLiteral("Occupied"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(manager.saveProject());
    const HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(ordinaryPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    auto release = qScopeGuard([&] { CloseHandle(occupied); });
    QMap<QString, QByteArray> committed;
    QDirIterator files(projectDirectory_, {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
        QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        committed.insert(path, readFile(path));
    }
    auto* folder = manager.currentProject()->findFolderByPath(
        QStringLiteral("Layers"), QStringLiteral("Occupied"));
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(manager.deleteEntries({}, {folder}));
    manager.currentProject()->name = QStringLiteral("Pending failed save");
    EXPECT_FALSE(manager.saveProject());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    EXPECT_EQ(readFile(ordinaryPath), QByteArray("retained bytes"));
    EXPECT_TRUE(QDir(emptyPath).exists());
}
#endif

}
