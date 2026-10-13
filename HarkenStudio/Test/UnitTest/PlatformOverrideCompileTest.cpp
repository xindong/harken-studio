#include "UnitTest/StudioObjectIdTestSupport.h"
#include "CurrentWorldArtifactWriter.h"
#include "CurrentWorldProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonValue>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/AITestSupport.h"

namespace VNX {
namespace {

constexpr uint8_t kVolumeId = 1;
constexpr uint8_t kLoopCountId = 4;

std::shared_ptr<ProjectItem> makeItem(const QString& name,
                                      ProjectItem::Type type,
                                      uint32_t objectId,
                                      harkenstudio::ObjectId stableId)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    item->attachDefaultComponents();
    return item;
}

void appendChild(const std::shared_ptr<ProjectItem>& parent,
                 const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

ProjectData makeRandomProject(std::shared_ptr<ProjectItem>* outContainer,
                              std::shared_ptr<ProjectItem>* outFirstSound)
{
    ProjectData project = HarkenTest::projectWithPlatforms(
        {QStringLiteral("Windows"), QStringLiteral("Android"), QStringLiteral("iOS")});
    project.nextObjectID = 6;

    auto master = makeItem(QStringLiteral("Master"), ProjectItem::Type::Bus,
                           5, harkentest::StudioObjectId(0x4d4153544552));
    ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer,
                          1, harkentest::StudioObjectId(2));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto container = makeItem(QStringLiteral("Random"),
                              ProjectItem::Type::Container, 2, harkentest::StudioObjectId(3));
    auto first = makeItem(QStringLiteral("First"), ProjectItem::Type::Sound,
                          3, harkentest::StudioObjectId(4));
    auto second = makeItem(QStringLiteral("Second"), ProjectItem::Type::Sound,
                           4, harkentest::StudioObjectId(5));
    appendChild(layer, container);
    appendChild(container, first);
    appendChild(container, second);
    first->comp<VSound>()->sources.plain.entries.push_back(
        {QUuid::createUuid(), 101});
    first->sourceID = 101;
    second->comp<VSound>()->sources.plain.entries.push_back(
        {QUuid::createUuid(), 102});
    second->sourceID = 102;
    EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, second->guid));

    auto* selection = container->comp<VNX::VSelection>();
    EXPECT_NE(selection, nullptr);
    selection->mode = SelectionMode::Random;
    selection->behavior.discipline = RandomDiscipline::Draw;
    selection->behavior.avoidRepeat = 0;
    selection->selections.shared.find(0)->weight = 0;
    selection->selections.shared.find(1)->weight = 3;

    ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));
    if (outContainer != nullptr) *outContainer = container;
    if (outFirstSound != nullptr) *outFirstSound = first;
    return project;
}

const harkenprojectschema::InputNode* findNode(
    const HarkenEditor::Bridge::CurrentWorldSyncRequest& request,
    harkenstudio::ObjectId stableId)
{
    const auto it = std::find_if(
        request.project.nodes.begin(), request.project.nodes.end(),
        [&](const auto& node) { return node.guid == stableId; });
    if (it == request.project.nodes.end())
        return nullptr;
    return &*it;
}

std::shared_ptr<ProjectItem> makeFixedItem(const QString& name,
                                           ProjectItem::Type type,
                                           uint32_t objectId)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(static_cast<uint64_t>(objectId) + 1));
    item->attachDefaultComponents();
    return item;
}

ProjectData makeDualPlatformWorldProject(std::shared_ptr<ProjectItem>* outSound)
{
    ProjectData project = HarkenTest::projectWithPlatforms(
        {QStringLiteral("Windows"), QStringLiteral("Android"), QStringLiteral("iOS")});
    project.nextObjectID = 5;
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Linux")});

    auto master = makeFixedItem(QStringLiteral("Master"), ProjectItem::Type::Bus, 4);
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeFixedItem(QStringLiteral("Layer"), ProjectItem::Type::Layer, 1);
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto sound = makeFixedItem(QStringLiteral("Tone"), ProjectItem::Type::Sound, 2);
    appendChild(layer, sound);
    sound->comp<VSound>()->sources.plain.entries.push_back(
        {QUuid(static_cast<uint>(101), 0, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x01),
         101});
    sound->sourceID = 101;
    auto action = makeFixedItem(QStringLiteral("PlayTone"),
                                ProjectItem::Type::Action, 3);
    VActionStep play;
    play.type = VActionStep::Type::Play;
    play.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(play);
    appendChild(layer, action);

    ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));
    if (outSound != nullptr) *outSound = sound;
    return project;
}

std::unordered_map<harkenstudio::ObjectId, std::uint64_t> fixedSoundRevisions(
    const ProjectItem& sound)
{
    return {{sound.runtimeId(), 1}};
}

bool writeWorld(ProjectData& project,
                PlatformScope scope,
                const ProjectItem& sound,
                const QString& path,
                QString* error)
{
    return WriteCurrentWorldArtifactV1(
        project, scope, path, fixedSoundRevisions(sound), nullptr, error);
}

QByteArray readFileBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

} // namespace

TEST(PlatformOverrideCompile, DefinitionVolumeOverlayFollowsPlatform)
{
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(nullptr, &first);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*first);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *first, *locator, kVolumeId, QJsonValue(-6.0), std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *first, *locator, kVolumeId, QJsonValue(-12.0), HarkenTest::PlatformIds::Android));

    QString error;
    const auto android = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Android, &error);
    ASSERT_NE(android, nullptr) << error.toStdString();
    const auto* androidNode = findNode(*android, first->runtimeId());
    ASSERT_NE(androidNode, nullptr);
    EXPECT_TRUE(androidNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
    EXPECT_FLOAT_EQ(androidNode->volume, -12.0f);

    error.clear();
    const auto windows = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error);
    ASSERT_NE(windows, nullptr) << error.toStdString();
    const auto* windowsNode = findNode(*windows, first->runtimeId());
    ASSERT_NE(windowsNode, nullptr);
    EXPECT_TRUE(windowsNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
    EXPECT_FLOAT_EQ(windowsNode->volume, -6.0f);

    error.clear();
    const auto shared = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error);
    ASSERT_NE(shared, nullptr) << error.toStdString();
    const auto* sharedNode = findNode(*shared, first->runtimeId());
    ASSERT_NE(sharedNode, nullptr);
    EXPECT_TRUE(sharedNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
    EXPECT_FLOAT_EQ(sharedNode->volume, -6.0f);
}

TEST(PlatformOverrideCompile, ContainerChildHasNoPlaybackPropertyOverridesAcrossPlatforms)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    const auto locator = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, first->guid);
    ASSERT_TRUE(locator.has_value());
    auto expectOccurrence = [&](PlatformScope scope) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, scope, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        const auto* node = findNode(*request, container->runtimeId());
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(node->containerChildOccurrences.size(), 2u);
        EXPECT_EQ(node->containerChildOccurrences[0].stableOccurrenceOrdinal,
                  locator->stableOccurrenceOrdinal);
        EXPECT_EQ(node->containerChildOccurrences[0].presence.bits, 0u);
        EXPECT_FLOAT_EQ(node->containerChildOccurrences[0].volume, 0.0f);
        ASSERT_EQ(request->containers.size(), 1u);
        ASSERT_EQ(request->containers.front().children.size(), 2u);
        EXPECT_EQ(request->containers.front().children[0].frequencyWeight, 0u);
        EXPECT_EQ(request->containers.front().children[1].frequencyWeight, 3u);
    };

    expectOccurrence(HarkenTest::PlatformIds::Android);
    expectOccurrence(HarkenTest::PlatformIds::Windows);
    expectOccurrence(std::nullopt);
}

TEST(PlatformOverrideCompile, LegacyTrackClipVolumeIsIgnoredAcrossPlatforms)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    ASSERT_EQ(container->children.size(), 2u);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto track = makeItem(QStringLiteral("Track"), ProjectItem::Type::Track, 4, harkentest::StudioObjectId(5));
    track->parent = container.get();
    container->children[1] = track;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, track->guid));
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    auto trackSound = makeItem(QStringLiteral("TrackSound"), ProjectItem::Type::Sound, 6, harkentest::StudioObjectId(6));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources.plain.entries = sound->comp<VSound>()->sources.plain.entries;
    appendChild(track, trackSound);
    project.nextObjectID = 7;
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = trackSound->guid;
    clip.start = 2000000;
    clip.duration = 1000000;
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);

    const auto locator = PlayablePropertyAuthoringV1::trackClipLocator(
        *track, clip.clipGuid);
    ASSERT_TRUE(locator.has_value());
    auto& occurrence = timeline->clips.items.front();
    ASSERT_TRUE(occurrence.playableProperties.setValue(Property::Volume, -6.0f));
    occurrence.playableProperties.presence.Set(Property::Volume);
    auto& overlay = occurrence.platformOverrides.ensure(HarkenTest::PlatformIds::Android)->properties;
    ASSERT_TRUE(overlay.setValue(Property::Volume, -12.0f));
    overlay.presence.Set(Property::Volume);

    auto expectClip = [&](PlatformScope scope) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, scope, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        const auto* node = findNode(*request, track->runtimeId());
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(node->clips.size(), 1u);
        EXPECT_EQ(node->clips[0].stableOccurrenceOrdinal, locator->stableOccurrenceOrdinal);
        EXPECT_EQ(node->clips[0].time, clip.start);
        EXPECT_EQ(node->clips[0].guid, clip.runtimeId());
        EXPECT_EQ(node->clips[0].presence.bits, 0u);
        EXPECT_FLOAT_EQ(node->clips[0].volume, 0.0f);
    };

    expectClip(HarkenTest::PlatformIds::Android);
    expectClip(HarkenTest::PlatformIds::Windows);
    expectClip(std::nullopt);
}

TEST(PlatformOverrideCompile, EmptyOverlayMatchesAcrossPlatforms)
{
    ProjectData project = makeRandomProject(nullptr, nullptr);

    QString error;
    const auto android = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Android, &error);
    ASSERT_NE(android, nullptr) << error.toStdString();
    error.clear();
    const auto windows = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error);
    ASSERT_NE(windows, nullptr) << error.toStdString();
    error.clear();
    const auto ios = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::iOS, &error);
    ASSERT_NE(ios, nullptr) << error.toStdString();
    error.clear();
    const auto shared = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error);
    ASSERT_NE(shared, nullptr) << error.toStdString();

    ASSERT_EQ(android->project.nodes.size(), windows->project.nodes.size());
    ASSERT_EQ(android->project.nodes.size(), ios->project.nodes.size());
    ASSERT_EQ(android->project.nodes.size(), shared->project.nodes.size());
    for (std::size_t i = 0; i < android->project.nodes.size(); ++i) {
        EXPECT_EQ(android->project.nodes[i].guid,
                  windows->project.nodes[i].guid);
        EXPECT_EQ(android->project.nodes[i].guid,
                  ios->project.nodes[i].guid);
        EXPECT_EQ(android->project.nodes[i].guid,
                  shared->project.nodes[i].guid);
        EXPECT_EQ(android->project.nodes[i].presence.bits,
                  windows->project.nodes[i].presence.bits);
        EXPECT_EQ(android->project.nodes[i].presence.bits,
                  ios->project.nodes[i].presence.bits);
        EXPECT_EQ(android->project.nodes[i].presence.bits,
                  shared->project.nodes[i].presence.bits);
        EXPECT_FLOAT_EQ(android->project.nodes[i].volume,
                        windows->project.nodes[i].volume);
        EXPECT_FLOAT_EQ(android->project.nodes[i].volume,
                        ios->project.nodes[i].volume);
        EXPECT_FLOAT_EQ(android->project.nodes[i].volume,
                        shared->project.nodes[i].volume);
        EXPECT_EQ(android->project.nodes[i].loopCount,
                  windows->project.nodes[i].loopCount);
        EXPECT_EQ(android->project.nodes[i].loopCount,
                  ios->project.nodes[i].loopCount);
        EXPECT_EQ(android->project.nodes[i].loopCount,
                  shared->project.nodes[i].loopCount);
        ASSERT_EQ(android->project.nodes[i].containerChildOccurrences.size(),
                  windows->project.nodes[i].containerChildOccurrences.size());
        ASSERT_EQ(android->project.nodes[i].containerChildOccurrences.size(),
                  shared->project.nodes[i].containerChildOccurrences.size());
        for (std::size_t j = 0;
             j < android->project.nodes[i].containerChildOccurrences.size();
             ++j) {
            EXPECT_EQ(
                android->project.nodes[i].containerChildOccurrences[j].presence.bits,
                windows->project.nodes[i].containerChildOccurrences[j].presence.bits);
            EXPECT_EQ(
                android->project.nodes[i].containerChildOccurrences[j].presence.bits,
                shared->project.nodes[i].containerChildOccurrences[j].presence.bits);
            EXPECT_FLOAT_EQ(
                android->project.nodes[i].containerChildOccurrences[j].volume,
                windows->project.nodes[i].containerChildOccurrences[j].volume);
            EXPECT_FLOAT_EQ(
                android->project.nodes[i].containerChildOccurrences[j].volume,
                shared->project.nodes[i].containerChildOccurrences[j].volume);
        }
    }
    ASSERT_EQ(android->containers.size(), windows->containers.size());
    ASSERT_EQ(android->containers.size(), shared->containers.size());
    for (std::size_t i = 0; i < android->containers.size(); ++i) {
        EXPECT_EQ(android->containers[i].queueCycle,
                  windows->containers[i].queueCycle);
        EXPECT_EQ(android->containers[i].queueCycle,
                  shared->containers[i].queueCycle);
        EXPECT_EQ(android->containers[i].queueCycleCount,
                  windows->containers[i].queueCycleCount);
        EXPECT_EQ(android->containers[i].queueCycleCount,
                  shared->containers[i].queueCycleCount);
    }
}

TEST(PlatformOverrideCompile, QueueLoopCountOverlayFollowsPlatform)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*container);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *container, *locator, kLoopCountId, QJsonValue(3), std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *container, *locator, kLoopCountId, QJsonValue(0), HarkenTest::PlatformIds::Android));

    QString error;
    const auto android = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Android, &error);
    ASSERT_NE(android, nullptr) << error.toStdString();
    ASSERT_EQ(android->containers.size(), 1u);
    EXPECT_EQ(android->containers[0].queueCycle, harkenstudiobuild::QueueCycleV1::Loop);
    EXPECT_EQ(android->containers[0].queueCycleCount, 0u);

    error.clear();
    const auto windows = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error);
    ASSERT_NE(windows, nullptr) << error.toStdString();
    ASSERT_EQ(windows->containers.size(), 1u);
    EXPECT_EQ(windows->containers[0].queueCycle, harkenstudiobuild::QueueCycleV1::Loop);
    EXPECT_EQ(windows->containers[0].queueCycleCount, 3u);

    error.clear();
    const auto shared = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error);
    ASSERT_NE(shared, nullptr) << error.toStdString();
    ASSERT_EQ(shared->containers.size(), 1u);
    EXPECT_EQ(shared->containers[0].queueCycle, harkenstudiobuild::QueueCycleV1::Loop);
    EXPECT_EQ(shared->containers[0].queueCycleCount, 3u);
}

TEST(PlatformOverrideCompile, DefinitionLoopCountOverlayBypassesTailGuard)
{
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(nullptr, &first);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*first);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *first, *locator, kLoopCountId, QJsonValue(2), std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *first, *locator, kLoopCountId, QJsonValue(7), HarkenTest::PlatformIds::Android));

    QString error;
    const auto android = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Android, &error);
    ASSERT_NE(android, nullptr) << error.toStdString();
    const auto* androidNode = findNode(*android, first->runtimeId());
    ASSERT_NE(androidNode, nullptr);
    EXPECT_TRUE(androidNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount));
    EXPECT_EQ(androidNode->loopCount, 7u);

    error.clear();
    const auto windows = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error);
    ASSERT_NE(windows, nullptr) << error.toStdString();
    const auto* windowsNode = findNode(*windows, first->runtimeId());
    ASSERT_NE(windowsNode, nullptr);
    EXPECT_TRUE(windowsNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount));
    EXPECT_EQ(windowsNode->loopCount, 2u);

    error.clear();
    const auto shared = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error);
    ASSERT_NE(shared, nullptr) << error.toStdString();
    const auto* sharedNode = findNode(*shared, first->runtimeId());
    ASSERT_NE(sharedNode, nullptr);
    EXPECT_TRUE(sharedNode->presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount));
    EXPECT_EQ(sharedNode->loopCount, 2u);
}

TEST(PlatformOverrideCompile, UniversalScopeIsRejected)
{
    ProjectData project = makeRandomProject(nullptr, nullptr);
    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Universal, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_FALSE(error.isEmpty());
}

TEST(PlatformOverrideCompile, EmptyOverlayWorldArtifactsMatchAcrossScopes)
{
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeDualPlatformWorldProject(&sound);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sharedPath = dir.filePath(QStringLiteral("shared.vnworld"));
    const QString windowsPath = dir.filePath(QStringLiteral("windows.vnworld"));
    const QString linuxPath = dir.filePath(QStringLiteral("linux.vnworld"));

    QString error;
    ASSERT_TRUE(writeWorld(project, std::nullopt, *sound, sharedPath, &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Windows, *sound, windowsPath,
                           &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Linux, *sound, linuxPath, &error))
        << error.toStdString();

    const QByteArray sharedBytes = readFileBytes(sharedPath);
    const QByteArray windowsBytes = readFileBytes(windowsPath);
    const QByteArray linuxBytes = readFileBytes(linuxPath);
    ASSERT_FALSE(sharedBytes.isEmpty());
    EXPECT_EQ(windowsBytes, sharedBytes);
    EXPECT_EQ(linuxBytes, sharedBytes);
}

TEST(PlatformOverrideCompile, LinuxVolumeOverlayForksOnlyLinuxWorldArtifact)
{
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeDualPlatformWorldProject(&sound);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sharedBeforePath =
        dir.filePath(QStringLiteral("shared-before.vnworld"));
    const QString windowsBeforePath =
        dir.filePath(QStringLiteral("windows-before.vnworld"));
    const QString linuxBeforePath =
        dir.filePath(QStringLiteral("linux-before.vnworld"));

    QString error;
    ASSERT_TRUE(writeWorld(project, std::nullopt, *sound, sharedBeforePath,
                           &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Windows, *sound,
                           windowsBeforePath, &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Linux, *sound, linuxBeforePath,
                           &error))
        << error.toStdString();
    const QByteArray sharedBefore = readFileBytes(sharedBeforePath);
    const QByteArray windowsBefore = readFileBytes(windowsBeforePath);
    const QByteArray linuxBefore = readFileBytes(linuxBeforePath);
    ASSERT_FALSE(sharedBefore.isEmpty());
    ASSERT_EQ(windowsBefore, sharedBefore);
    ASSERT_EQ(linuxBefore, sharedBefore);

    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, QJsonValue(-12.0), HarkenTest::PlatformIds::Linux));

    const QString sharedAfterPath =
        dir.filePath(QStringLiteral("shared-after.vnworld"));
    const QString windowsAfterPath =
        dir.filePath(QStringLiteral("windows-after.vnworld"));
    const QString linuxAfterPath =
        dir.filePath(QStringLiteral("linux-after.vnworld"));
    error.clear();
    ASSERT_TRUE(writeWorld(project, std::nullopt, *sound, sharedAfterPath,
                           &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Windows, *sound,
                           windowsAfterPath, &error))
        << error.toStdString();
    error.clear();
    ASSERT_TRUE(writeWorld(project, HarkenTest::PlatformIds::Linux, *sound, linuxAfterPath,
                           &error))
        << error.toStdString();

    const QByteArray sharedAfter = readFileBytes(sharedAfterPath);
    const QByteArray windowsAfter = readFileBytes(windowsAfterPath);
    const QByteArray linuxAfter = readFileBytes(linuxAfterPath);
    EXPECT_EQ(windowsAfter, sharedAfter);
    EXPECT_NE(linuxAfter, linuxBefore);
    EXPECT_NE(linuxAfter, sharedAfter);
    EXPECT_EQ(windowsAfter, windowsBefore);
    EXPECT_EQ(sharedAfter, sharedBefore);
}

} // namespace VNX

namespace VNX {
namespace {

class PlatformOverrideCompileBusScope : public ::testing::Test {
protected:
    void SetUp() override {
        TestSupport::ensureApp();
        ASSERT_TRUE(directory_.isValid());
        project_ = makeRandomProject(nullptr, nullptr);
        previousPlatform_ = PlatformContextService::instance().context();
        auto& manager = ProjectManager::instance();
        ASSERT_EQ(manager.currentProject(), nullptr);
        ProjectConfig config;
        config.name = QStringLiteral("BusScope");
        config.location = directory_.path();
        config.platforms = project_.platforms;
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
        ASSERT_EQ(PlatformContextService::instance().context().primary,
                  HarkenTest::PlatformIds::Windows);
    }

    void TearDown() override {
        auto& manager = ProjectManager::instance();
        if (manager.currentProject()) manager.closeProject();
        PlatformContextService::instance().setSelected(
            previousPlatform_.selected, previousPlatform_.primary, previousPlatform_.all);
    }

    void setPitch(float shared, float windows) {
        auto master = project_.busses.front().rootItem;
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*master);
        ASSERT_TRUE(locator.has_value());
        const auto pitch = static_cast<uint8_t>(
            harkenplayableproperties::PlayablePropertyIdV1::Pitch);
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            *master, *locator, pitch, QJsonValue(shared), std::nullopt));
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            *master, *locator, pitch, QJsonValue(windows), HarkenTest::PlatformIds::Windows));
    }

    QTemporaryDir directory_;
    ProjectData project_;
    PlatformContext previousPlatform_;
};

TEST_F(PlatformOverrideCompileBusScope,
       SharedPitchRejectsNonNeutralBaseDespiteNeutralSelectedPlatform) {
    ASSERT_NO_FATAL_FAILURE(setPitch(300.0f, 0.0f));
    QString error;
    EXPECT_EQ(buildCurrentWorldSyncRequest(project_, std::nullopt, &error), nullptr);
    EXPECT_TRUE(error.contains(QStringLiteral("pitch"))) << error.toStdString();
    EXPECT_NE(buildCurrentWorldSyncRequest(project_, HarkenTest::PlatformIds::Windows), nullptr);
    EXPECT_TRUE(HarkenEditor::Bridge::canonicalRuntimeProjectContext(project_).ok());
}

TEST_F(PlatformOverrideCompileBusScope,
       SharedPitchAllowsNeutralBaseDespiteNonNeutralSelectedPlatform) {
    ASSERT_NO_FATAL_FAILURE(setPitch(0.0f, 300.0f));
    QString error;
    EXPECT_NE(buildCurrentWorldSyncRequest(project_, std::nullopt, &error), nullptr)
        << error.toStdString();
    EXPECT_EQ(buildCurrentWorldSyncRequest(project_, HarkenTest::PlatformIds::Windows), nullptr);
    EXPECT_FALSE(HarkenEditor::Bridge::canonicalRuntimeProjectContext(project_).ok());
}

TEST_F(PlatformOverrideCompileBusScope,
       VolumeAndSendFollowSharedOrExplicitPlatformInsteadOfSelectedPlatform) {
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    auto master = project_.busses.front().rootItem;
    auto source = makeItem(QStringLiteral("Source"), ProjectItem::Type::Bus,
                           6, harkentest::StudioObjectId(6));
    auto destination = makeItem(QStringLiteral("Destination"), ProjectItem::Type::Bus,
                                7, harkentest::StudioObjectId(7));
    appendChild(master, source);
    appendChild(master, destination);
    project_.nextObjectID = 8;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*source);
    ASSERT_TRUE(locator.has_value());
    const auto set = [&](PlatformScope scope, float volume, float send) {
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            *source, *locator, static_cast<uint8_t>(Property::Volume),
            QJsonValue(volume), scope));
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            *source, *locator, static_cast<uint8_t>(Property::Sends),
            QJsonArray{QJsonObject{{QStringLiteral("targetBusID"),
                                   static_cast<int>(destination->objectID)},
                                  {QStringLiteral("sendLevel"), send}}}, scope));
    };
    ASSERT_NO_FATAL_FAILURE(set(std::nullopt, -6.0f, -3.0f));
    ASSERT_NO_FATAL_FAILURE(set(HarkenTest::PlatformIds::Windows, -12.0f, -9.0f));
    ASSERT_NO_FATAL_FAILURE(set(HarkenTest::PlatformIds::Android, 0.0f, 0.0f));
    const auto verify = [&](const auto& result, float volume, float send) {
        ASSERT_NE(result, nullptr);
        const auto bus = std::find_if(result->buses.begin(), result->buses.end(),
            [&](const auto& row) { return row.busId == source->runtimeId(); });
        ASSERT_NE(bus, result->buses.end());
        EXPECT_FLOAT_EQ(bus->faderDb, volume);
        ASSERT_EQ(result->sends.size(), 1u);
        EXPECT_FLOAT_EQ(result->sends.front().sendLevelDb, send);
    };
    for (const auto& [scope, volume, send] : {
             std::tuple{PlatformScope{}, -6.0f, -3.0f},
             std::tuple{PlatformScope{HarkenTest::PlatformIds::Windows}, -12.0f, -9.0f},
             std::tuple{PlatformScope{HarkenTest::PlatformIds::Android}, 0.0f, 0.0f}}) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project_, scope, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_NO_FATAL_FAILURE(verify(request->busResult, volume, send));
    }
    const auto selected = HarkenEditor::Bridge::canonicalRuntimeProjectContext(project_);
    ASSERT_TRUE(selected.ok());
    ASSERT_NO_FATAL_FAILURE(verify(selected.busResult, -12.0f, -9.0f));
}

} // namespace
} // namespace VNX
