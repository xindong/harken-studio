#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"
#include "PackBuildPlan.h"
#include "PackExportSelection.h"
#include "ProjectPackExporter.h"
#include "CanonicalPackSetExport.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/PackagingBridge.h"

#include "harken/Harken.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

namespace {

constexpr uint64_t kStableIdBias = 100000;

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeTone(const QString& path, float amplitude)
{
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 48000;
    constexpr uint32_t frames = 96;
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i)
        samples[i] = amplitude * static_cast<float>(i % 13) / 16.0f;

    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);

    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

uint32_t registerTone(const QString& root, const QString& relativePath,
                      float amplitude)
{
    const QString absolutePath =
        QDir(VNX::PathUtils::getAudioSourcesDir(root)).filePath(relativePath);
    writeTone(absolutePath, amplitude);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = absolutePath;
    info.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    info.metadata.sampleRate = 48000;
    info.metadata.channels = 1;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = 96;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(absolutePath).size());
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(absolutePath).lastModified().toSecsSinceEpoch());
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectId)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(type == VNX::ProjectItem::Type::Bus
        ? harkentest::StudioObjectId(0x4d4153544552) : harkentest::StudioObjectId(objectId + kStableIdBias));
    item->attachDefaultComponents();
    return item;
}

void attach(const std::shared_ptr<VNX::ProjectItem>& parent,
            const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    child->parentRef = {VNX::ParentRef::Kind::Item, parent->guid};
    parent->children.push_back(child);
}

void addBinding(std::vector<VNX::ProjectFileBinding>& bindings,
                const std::shared_ptr<VNX::ProjectItem>& root)
{
    VNX::ProjectFileBinding binding;
    binding.rootItem = root;
    bindings.push_back(std::move(binding));
}

VNX::VActionStep step(VNX::VActionStep::Type type,
                      const VNX::ProjectItem& target)
{
    VNX::VActionStep result;
    result.type = type;
    result.targetObjectID = target.objectID;
    result.targetName = target.name;
    return result;
}

bool hasEdge(const VNX::ActionClosurePlan& closure,
             VNX::ActionClosureEdgeReason reason, harkenstudio::ObjectId target)
{
    return std::any_of(closure.edges.begin(), closure.edges.end(),
                       [&](const auto& edge) {
        return edge.reason == reason && edge.toStableId == target;
    });
}

bool hasMember(const std::vector<VNX::ActionClosureMember>& members,
               harkenstudio::ObjectId stableId)
{
    return std::any_of(members.begin(), members.end(), [&](const auto& member) {
        return member.stableId == stableId;
    });
}

bool hasCapability(const VNX::ActionClosurePlan& closure,
                   VNX::RuntimeCapabilityFeature feature,
                   VNX::RuntimeCapabilityState state)
{
    return std::any_of(closure.capabilities.begin(), closure.capabilities.end(),
                       [&](const auto& capability) {
        return capability.feature == feature && capability.state == state;
    });
}

struct PackImage
{
    QByteArray bytes;
    harken::PackV2Header header;
    std::vector<harken::PackV2DirectoryEntry> entries;

    const uint8_t* data() const
    {
        return reinterpret_cast<const uint8_t*>(bytes.constData());
    }

    int countKind(uint32_t kind) const
    {
        return static_cast<int>(std::count_if(
            entries.begin(), entries.end(),
            [kind](const auto& entry) { return entry.kind == kind; }));
    }
};

std::map<harkenstudio::ObjectId, harken::PackTocEntry> mediaBySoundId(const PackImage& image)
{
    std::map<harkenstudio::ObjectId, harken::PackTocEntry> result;
    for (const auto& entry : image.entries) {
        if (entry.kind != harken::kPackEntryKindMedia)
            continue;
        const auto media = harken::ParsePackTocEntry(image.data() + entry.offset);
        result.emplace(harkentest::ToStudio(media.soundId), media);
    }
    return result;
}

struct PackRuntimeGuard
{
    QByteArray previousPackcPath = qgetenv("HARKEN_PACKC_PATH");
    bool hadPackcPath = qEnvironmentVariableIsSet("HARKEN_PACKC_PATH");
    bool initialized = false;

    ~PackRuntimeGuard()
    {
        if (initialized) Vn_Term();
        VNX::AssetRegistry::instance().clear();
        if (hadPackcPath)
            qputenv("HARKEN_PACKC_PATH", previousPackcPath);
        else
            qunsetenv("HARKEN_PACKC_PATH");
    }
};

void readPack(const QString& path, PackImage& image)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    image.bytes = file.readAll();
    ASSERT_GE(image.bytes.size(),
              static_cast<qsizetype>(harken::kPackV2HeaderBytes));
    ASSERT_TRUE(harken::ParsePackV2Header(image.data(), &image.header));
    for (uint32_t i = 0; i < image.header.entryCount; ++i)
        image.entries.push_back(harken::ParsePackV2DirectoryEntry(
            image.data() + image.header.directoryOffset
            + i * harken::kPackV2DirectoryEntryBytes));
}

QJsonArray readPublication(const QString& outputDir)
{
    QJsonArray packs;
    QStringList files;
    QString error;
    EXPECT_TRUE(VNX::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error))
        << error.toStdString();
    return packs;
}

QJsonObject findNamed(const QJsonArray& array, const QString& name)
{
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("name")).toString() == name)
            return object;
    }
    return {};
}

QString variantPackPath(const VNX::ProjectData& project, const QString& outputDir, harkenstudio::ObjectId stableId,
                        const QString& /*platform*/,
                        const QString& language = QString())
{
    for (const auto& unit : VNX::ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.stableId == stableId)
            return QDir(outputDir).filePath(VNX::PackBuildPlanning::packFileName(
                language, unit.name, QStringLiteral(".bytes")));
    ADD_FAILURE() << "Action not found in test project";
    return {};
}

QStringList publicationPlan(const QString& outputDir, const QString& actionName)
{
    QStringList result;
    const QJsonArray packs = readPublication(outputDir);
    const QJsonObject initPack = findNamed(packs, QStringLiteral("Init"));
    EXPECT_FALSE(initPack.isEmpty());
    result.append(QDir(outputDir).filePath(
        QFileInfo(initPack.value(QStringLiteral("path")).toString()).fileName()));
    const QJsonObject action = findNamed(packs, actionName);
    EXPECT_FALSE(action.isEmpty());
    for (const QJsonValue& value :
         action.value(QStringLiteral("dependencies")).toArray()) {
        const QString token = value.toString();
        for (const QJsonValue& packValue : packs) {
            const QJsonObject dependency = packValue.toObject();
            if (dependency.value(QStringLiteral("packStableId")).toString() != token)
                continue;
            const QString path = QDir(outputDir).filePath(QFileInfo(
                dependency.value(QStringLiteral("path")).toString()).fileName());
            if (!result.contains(path))
                result.append(path);
        }
    }
    result.append(QDir(outputDir).filePath(
        QFileInfo(action.value(QStringLiteral("path")).toString()).fileName()));
    return result;
}

VnResult loadPack(const QString& path, VnPackHandle* handle)
{
    VnPackDesc descriptor{};
    descriptor.structSize = sizeof(descriptor);
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    descriptor.utf8Path = utf8.constData();
    return Vn_LoadPack(&descriptor, handle);
}

VNX::ProjectData executableProject(const QString& root, uint32_t sharedSourceId)
{
    VNX::ProjectData project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("phase6-pack-scenario");
    project.projectPath = QDir(root).filePath(QStringLiteral("scenario.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Linux")});

    const VNX::AssetInfo* asset =
        VNX::AssetRegistry::instance().getAsset(sharedSourceId);
    EXPECT_NE(asset, nullptr);
    if (asset) {
        VNX::AudioSourceData source;
        source.sourceID = sharedSourceId;
        source.relativePath = asset->relativePath;
        source.language = QStringLiteral("SFX");
        project.audioSources.push_back(source);
    }

    auto master = makeItem(QStringLiteral("MasterBus"),
                           VNX::ProjectItem::Type::Bus, 200);
    addBinding(project.busses, master);

    auto layer = makeItem(QStringLiteral("Root"),
                          VNX::ProjectItem::Type::Layer, 10);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    auto sound = makeItem(QStringLiteral("SharedTone"),
                          VNX::ProjectItem::Type::Sound, 1);
    HarkenTest::setSharedSoundSource(*sound, sharedSourceId);
    sound->comp<VNX::VMixNode>()->outputBusId = master->objectID;
    attach(layer, sound);
    addBinding(project.layers, layer);

    auto actions = makeItem(QStringLiteral("Actions"),
                            VNX::ProjectItem::Type::None, 20);
    auto head = makeItem(QStringLiteral("Head"),
                         VNX::ProjectItem::Type::Action, 21);
    auto tail = makeItem(QStringLiteral("Tail"),
                         VNX::ProjectItem::Type::Action, 22);
    auto peer = makeItem(QStringLiteral("Peer"),
                         VNX::ProjectItem::Type::Action, 23);
    head->comp<VNX::VAction>()->steps = {
        step(VNX::VActionStep::Type::Play, *sound),
        step(VNX::VActionStep::Type::PostAction, *tail)};
    tail->comp<VNX::VAction>()->steps = {
        step(VNX::VActionStep::Type::Play, *sound)};
    peer->comp<VNX::VAction>()->steps = {
        step(VNX::VActionStep::Type::Play, *sound)};
    attach(actions, head);
    attach(actions, tail);
    attach(actions, peer);
    addBinding(project.actions, actions);

    return project;
}

}

TEST(PackBuildScenario, BuildPlanCarriesVariantSelectionAndCompleteActionClosure)
{
    auto localized = makeItem(QStringLiteral("Localized"),
                              VNX::ProjectItem::Type::Sound, 1);
    auto* localizedSound = localized->comp<VNX::VSound>();
    ASSERT_NE(localizedSound, nullptr);

    struct SourceRow
    {
        uint32_t sourceId;
        QString language;
    };
    const std::vector<SourceRow> rows = {
        {11, QString()},
        {12, QString()},
        {13, QString()},
        {14, QStringLiteral("Japanese")},
        {15, QStringLiteral("Japanese")}};
    for (const SourceRow& row : rows) {
        VNX::SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = row.sourceId;
        auto& set = row.language.isEmpty() ? localizedSound->sources.plain
            : localizedSound->sources.localized[row.language];
        set.entries.push_back(source);
    }
    localizedSound->sources.plain.platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = localizedSound->sources.plain.entries[1].entryId;
    localizedSound->sources.plain.platforms[HarkenTest::platformIdString(QStringLiteral("Linux"))].active = localizedSound->sources.plain.entries[2].entryId;

    auto track = makeItem(QStringLiteral("Track"),
                          VNX::ProjectItem::Type::Track, 2);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = localized->guid;
    clip.duration = 1;
    track->comp<VNX::VTrack>()->clips.items.push_back(clip);
    auto piece = makeItem(QStringLiteral("Piece"),
                          VNX::ProjectItem::Type::Piece, 3);
    attach(piece, track);

    auto gridTarget = makeItem(QStringLiteral("GridTarget"),
                               VNX::ProjectItem::Type::Sound, 4);
    auto bridge = makeItem(QStringLiteral("Bridge"),
                           VNX::ProjectItem::Type::Piece, 5);
    auto endpoint = makeItem(QStringLiteral("Endpoint"),
                             VNX::ProjectItem::Type::Sound, 6);
    auto container = makeItem(QStringLiteral("Container"),
                              VNX::ProjectItem::Type::Container, 7);
    attach(container, gridTarget);
    attach(container, endpoint);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    ASSERT_TRUE(selection->selections.shared.setOccurrenceRows({{gridTarget->guid, 0}, {endpoint->guid, 1}}));
    selection->mode = VNX::SelectionMode::Match;
    selection->matchResponse = VNX::VariableResponse::Live;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(QUuid::createUuid()));
        ASSERT_TRUE(grid.addRow(gridTarget->guid));
        EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->transitions.rules.front().bridge = bridge->guid;

    auto clock = makeItem(QStringLiteral("Clock"),
                          VNX::ProjectItem::Type::Piece, 8);
    auto variable = makeItem(QStringLiteral("Variable"),
                             VNX::ProjectItem::Type::Variable, 9);
    auto tail = makeItem(QStringLiteral("Tail"),
                         VNX::ProjectItem::Type::Action, 10);
    tail->comp<VNX::VAction>()->steps.push_back(
        step(VNX::VActionStep::Type::Play, *localized));
    auto root = makeItem(QStringLiteral("RootAction"),
                         VNX::ProjectItem::Type::Action, 11);
    VNX::VActionStep playPiece = step(VNX::VActionStep::Type::Play, *piece);
    playPiece.quantize.referenceObjectID = clock->objectID;
    playPiece.quantize.referenceName = clock->name;
    root->comp<VNX::VAction>()->steps = {
        playPiece,
        step(VNX::VActionStep::Type::Play, *container),
        step(VNX::VActionStep::Type::SetVariable, *variable),
        step(VNX::VActionStep::Type::PostAction, *tail)};

    VNX::ProjectData project = HarkenTest::projectWithPlatforms();
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Linux")});
    project.projectLanguages = {QStringLiteral("Japanese")};
    addBinding(project.layers, localized);
    addBinding(project.layers, piece);
    addBinding(project.layers, bridge);
    addBinding(project.layers, container);
    addBinding(project.layers, clock);
    addBinding(project.variables, variable);
    addBinding(project.actions, tail);
    addBinding(project.actions, root);

    VNX::PackExportSelection exportSelection;
    exportSelection.actionTargeting = {{root->runtimeId(), false,
        {HarkenTest::platformIdString(QStringLiteral("Windows"))}}};

    const VNX::PackBuildPlan plan = VNX::PackBuildPlanning::build(
        project, {}, {}, false, exportSelection);
    ASSERT_EQ(plan.variants.size(), 3u);
    EXPECT_TRUE(plan.groupMetadata.value(QStringLiteral("groups")).toArray().isEmpty());

    const auto rootVariant = [&](const QString& platform,
                                 const QString& language) {
        return std::find_if(plan.variants.begin(), plan.variants.end(),
                            [&](const auto& variant) {
            return variant.actionStableId == root->runtimeId()
                && variant.variant.platform == platform
                && variant.variant.language == language;
        });
    };

    const auto windows =
        rootVariant(HarkenTest::platformIdString(QStringLiteral("Windows")), QString());
    ASSERT_NE(windows, plan.variants.end());
    const auto& closure = windows->closure;
    EXPECT_TRUE(hasMember(closure.actions, tail->runtimeId()));
    EXPECT_TRUE(hasMember(closure.objects, piece->runtimeId()));
    EXPECT_TRUE(hasMember(closure.objects, track->runtimeId()));
    EXPECT_TRUE(hasMember(closure.objects, bridge->runtimeId()));
    EXPECT_TRUE(hasMember(closure.objects, gridTarget->runtimeId()));
    EXPECT_FALSE(hasMember(closure.objects, clock->runtimeId()));
    EXPECT_FALSE(hasMember(closure.objects, variable->runtimeId()));
    EXPECT_TRUE(hasEdge(closure, VNX::ActionClosureEdgeReason::PostAction,
                        tail->runtimeId()));
    EXPECT_TRUE(hasEdge(closure, VNX::ActionClosureEdgeReason::TrackClip,
                        localized->runtimeId()));
    EXPECT_TRUE(hasEdge(closure, VNX::ActionClosureEdgeReason::GridRow,
                        gridTarget->runtimeId()));
    EXPECT_TRUE(hasEdge(closure, VNX::ActionClosureEdgeReason::TransitionBridge,
                        bridge->runtimeId()));
    EXPECT_FALSE(hasEdge(closure,
                         VNX::ActionClosureEdgeReason::QuantizeReference,
                         clock->runtimeId()));
    EXPECT_EQ(windows->dependencyOrder,
              (std::vector<harkenstudio::ObjectId>{tail->runtimeId(),
                                     root->runtimeId()}));

    std::set<uint32_t> selectedSourceIds;
    for (const auto& media : closure.media)
        selectedSourceIds.insert(media.authoringSourceId);
    EXPECT_EQ(selectedSourceIds, (std::set<uint32_t>{12}));
    EXPECT_EQ(selectedSourceIds.count(11), 0u);
    EXPECT_EQ(selectedSourceIds.count(14), 0u);

    EXPECT_EQ(rootVariant(HarkenTest::platformIdString(QStringLiteral("Linux")), QString()), plan.variants.end());
    const auto linux = std::find_if(plan.variants.begin(), plan.variants.end(), [&](const auto& variant) {
        return variant.actionStableId == tail->runtimeId()
            && variant.variant.platform == HarkenTest::platformIdString(QStringLiteral("Linux"));
    });
    ASSERT_NE(linux, plan.variants.end());
    selectedSourceIds.clear();
    for (const auto& media : linux->closure.media)
        selectedSourceIds.insert(media.authoringSourceId);
    EXPECT_EQ(selectedSourceIds, (std::set<uint32_t>{13}));
    EXPECT_EQ(selectedSourceIds.count(11), 0u);
    EXPECT_EQ(selectedSourceIds.count(15), 0u);
    EXPECT_TRUE(hasCapability(
        closure, VNX::RuntimeCapabilityFeature::Transition,
        VNX::RuntimeCapabilityState::InvalidAuthoring));
    EXPECT_FALSE(closure.ready());
}

TEST(PackBuildScenario, PhysicalLocalizedVariantsUseSelectedAndCommonMedia)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t localizedEnglishSource =
        registerTone(dir.path(), QStringLiteral("localized-en.wav"), 0.25f);
    const uint32_t localizedJapaneseSource =
        registerTone(dir.path(), QStringLiteral("localized-ja.wav"), 0.75f);
    const uint32_t commonSource =
        registerTone(dir.path(), QStringLiteral("common.wav"), 1.0f);

    VNX::ProjectData project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("localized-variants");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("localized.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    for (const auto& [sourceId, path, language] :
         std::vector<std::tuple<uint32_t, QString, QString>>{
             {localizedEnglishSource, QStringLiteral("localized-en.wav"), QStringLiteral("English")},
             {localizedJapaneseSource, QStringLiteral("localized-ja.wav"), QStringLiteral("Japanese")},
             {commonSource, QStringLiteral("common.wav"), QString()}}) {
        VNX::AudioSourceData source;
        source.sourceID = sourceId;
        source.relativePath = path;
        source.language = language;
        project.audioSources.push_back(source);
    }
    auto master = makeItem(QStringLiteral("MasterBus"),
                           VNX::ProjectItem::Type::Bus, 200);
    addBinding(project.busses, master);

    auto layer = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::Layer, 10);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    auto localized = makeItem(QStringLiteral("Localized"), VNX::ProjectItem::Type::Sound, 1);
    auto common = makeItem(QStringLiteral("Common"), VNX::ProjectItem::Type::Sound, 2);
    auto* localizedSound = localized->comp<VNX::VSound>();
    auto* commonSound = common->comp<VNX::VSound>();
    ASSERT_NE(localizedSound, nullptr);
    ASSERT_NE(commonSound, nullptr);

    VNX::SoundSourceDef localizedEnglish;
    localizedEnglish.entryId = QUuid::createUuid();
    localizedEnglish.sourceId = localizedEnglishSource;
    VNX::SoundSourceDef localizedJapanese;
    localizedJapanese.entryId = QUuid::createUuid();
    localizedJapanese.sourceId = localizedJapaneseSource;
    localizedSound->distinguishLanguage = true;
    localizedSound->sources.localized["English"].entries = {localizedEnglish};
    localizedSound->sources.localized["Japanese"].entries = {localizedJapanese};
    localizedSound->sources.localized[QStringLiteral("English")].platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = localizedEnglish.entryId;
    localizedSound->sources.localized[QStringLiteral("Japanese")].platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = localizedJapanese.entryId;

    VNX::SoundSourceDef commonRow;
    commonRow.entryId = QUuid::createUuid();
    commonRow.sourceId = commonSource;
    commonSound->sources.plain.entries = {commonRow};
    commonSound->sources.plain.platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = commonRow.entryId;
    attach(layer, localized);
    attach(layer, common);
    addBinding(project.layers, layer);

    auto action = makeItem(QStringLiteral("LocalizedAction"),
                           VNX::ProjectItem::Type::Action, 21);
    action->comp<VNX::VAction>()->steps = {
        step(VNX::VActionStep::Type::Play, *localized),
        step(VNX::VActionStep::Type::Play, *common)};
    addBinding(project.actions, action);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString(QStringLiteral("Windows"));
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString englishPath = variantPackPath(project,
        dir.path(), action->runtimeId(), HarkenTest::platformIdString(QStringLiteral("Windows")),
        QStringLiteral("English"));
    const QString japanesePath = variantPackPath(project,
        dir.path(), action->runtimeId(), HarkenTest::platformIdString(QStringLiteral("Windows")),
        QStringLiteral("Japanese"));
    EXPECT_EQ(QFileInfo(englishPath).fileName(), QStringLiteral("LocalizedAction.bytes"));
    EXPECT_EQ(QFileInfo(japanesePath).fileName(), QFileInfo(englishPath).fileName());
    EXPECT_NE(englishPath, japanesePath);
    PackImage englishImage;
    PackImage japaneseImage;
    ASSERT_NO_FATAL_FAILURE(readPack(englishPath, englishImage));
    ASSERT_NO_FATAL_FAILURE(readPack(japanesePath, japaneseImage));
    const auto englishMedia = mediaBySoundId(englishImage);
    const auto japaneseMedia = mediaBySoundId(japaneseImage);
    ASSERT_EQ(englishMedia.size(), 2u);
    ASSERT_EQ(japaneseMedia.size(), 2u);
    ASSERT_TRUE(englishMedia.contains(localized->runtimeId()));
    ASSERT_TRUE(japaneseMedia.contains(localized->runtimeId()));
    EXPECT_NE(englishMedia.at(localized->runtimeId()).blobChecksum,
              japaneseMedia.at(localized->runtimeId()).blobChecksum);
    EXPECT_EQ(englishMedia.at(common->runtimeId()).blobChecksum,
              japaneseMedia.at(common->runtimeId()).blobChecksum);

    const QJsonArray events = readPublication(dir.path());
    std::set<QString> manifestLanguages;
    std::set<QString> manifestPaths;
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("name")).toString()
            != QStringLiteral("LocalizedAction"))
            continue;
        manifestLanguages.insert(event.value(QStringLiteral("language")).toString());
        manifestPaths.insert(event.value(QStringLiteral("path")).toString());
    }
    EXPECT_EQ(manifestLanguages,
              (std::set<QString>{QStringLiteral("English"), QStringLiteral("Japanese")}));
    EXPECT_EQ(manifestPaths,
              (std::set<QString>{QStringLiteral("English/LocalizedAction.bytes"),
                                 QStringLiteral("Japanese/LocalizedAction.bytes")}));

    VNX::AssetRegistry::instance().clear();
}

TEST(PackBuildScenario, ExportAndRuntimeHonorActionOwnershipAndPlatformSelection)
{
    PackRuntimeGuard guard;
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sourceId =
        registerTone(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    VNX::ProjectData project = executableProject(dir.path(), sourceId);

    VNX::PackExportSelection exportSelection;
    exportSelection.actionTargeting = {{harkentest::StudioObjectId(21 + kStableIdBias), false,
        {HarkenTest::platformIdString(QStringLiteral("Windows"))}}};
    const auto plan = VNX::PackBuildPlanning::build(project, {}, {}, false, exportSelection);
    ASSERT_EQ(plan.units.size(), 5u);
    const auto headUnit = std::find_if(plan.units.begin(), plan.units.end(), [](const auto& unit) {
        return unit.actionStableIds == std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(21 + kStableIdBias)};
    });
    ASSERT_NE(headUnit, plan.units.end());
    EXPECT_EQ(headUnit->name, QStringLiteral("Head"));
    EXPECT_EQ(headUnit->variant.platform, HarkenTest::platformIdString(QStringLiteral("Windows")));
    EXPECT_EQ(headUnit->actionStableIds,
              (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(21 + kStableIdBias)}));

    const QString windowsDir = QDir(dir.path()).filePath(QStringLiteral("windows"));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.selection = exportSelection;
    options.platform = HarkenTest::platformIdString(QStringLiteral("Windows"));
    const auto windowsResult = exporter.ExportAllPacks(project, windowsDir, options);
    ASSERT_TRUE(windowsResult.success) << windowsResult.error.toStdString();

    const QString sharedPath =
        QDir(windowsDir).filePath(QStringLiteral("Shared.bytes"));
    const QString headPath = variantPackPath(project,
        windowsDir, harkentest::StudioObjectId(21 + kStableIdBias), HarkenTest::platformIdString(QStringLiteral("Windows")));
    const QString tailPath = variantPackPath(project,
        windowsDir, harkentest::StudioObjectId(22 + kStableIdBias), HarkenTest::platformIdString(QStringLiteral("Windows")));
    const QString peerPath = variantPackPath(project,
        windowsDir, harkentest::StudioObjectId(23 + kStableIdBias), HarkenTest::platformIdString(QStringLiteral("Windows")));
    EXPECT_FALSE(QFileInfo::exists(sharedPath));
    for (const QString& path : {headPath, tailPath, peerPath})
        ASSERT_TRUE(QFileInfo::exists(path)) << path.toStdString();

    PackImage headImage;
    PackImage tailImage;
    PackImage peerImage;
    ASSERT_NO_FATAL_FAILURE(readPack(headPath, headImage));
    ASSERT_NO_FATAL_FAILURE(readPack(tailPath, tailImage));
    ASSERT_NO_FATAL_FAILURE(readPack(peerPath, peerImage));
    EXPECT_EQ(headImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(tailImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(peerImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(headImage.countKind(10u), 0);
    EXPECT_EQ(tailImage.countKind(10u), 0);
    EXPECT_EQ(peerImage.countKind(10u), 0);

    const QStringList loadPlan = publicationPlan(windowsDir, QStringLiteral("Head"));
    ASSERT_EQ(loadPlan.size(), 2);
    EXPECT_EQ(QFileInfo(loadPlan[0]).fileName(), QStringLiteral("Init.bytes"));
    EXPECT_EQ(QFileInfo(loadPlan[1]).fileName(), QFileInfo(headPath).fileName());

    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    guard.initialized = true;
    std::vector<VnPackHandle> handles;
    for (const QString& path : loadPlan) {
        VnPackHandle handle = 0;
        ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(path, &handle)) << path.toStdString();
        handles.push_back(handle);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_Term());
    guard.initialized = false;

    const QString linuxDir = QDir(dir.path()).filePath(QStringLiteral("linux"));
    options.platform = HarkenTest::platformIdString(QStringLiteral("Linux"));
    const auto linuxResult = exporter.ExportAllPacks(project, linuxDir, options);
    ASSERT_TRUE(linuxResult.success) << linuxResult.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, linuxDir, harkentest::StudioObjectId(21 + kStableIdBias), HarkenTest::platformIdString(QStringLiteral("Linux")))));
    const QJsonArray linuxPublication = readPublication(linuxDir);
    EXPECT_TRUE(findNamed(linuxPublication,
                          QStringLiteral("Head")).isEmpty());
    EXPECT_FALSE(findNamed(linuxPublication,
                           QStringLiteral("Tail")).isEmpty());
}
