#include "UnitTest/StudioObjectIdTestSupport.h"
#include <harken_project_schema/FrontEnd.h>
#include <harken/core/CurrentWorldArtifact.h>
#include <harken/core/CurrentWorldCodec.h>
#include <harken/param/VariableService.h>

#include "CurrentWorldProjectAdapter.h"
#include "CurrentWorldArtifactWriter.h"
#include "CurrentWorldArtifactWriterInternal.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "ContainerBehaviorSemantics.h"
#include "Components/VAction.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <utility>

namespace VNX {
namespace {

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
        {QStringLiteral("Windows"), QStringLiteral("Android")});
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
    HarkenTest::setSharedSoundSource(*first, 101);
    HarkenTest::setSharedSoundSource(*second, 102);
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

enum class ArtifactFailureStage { Write, Commit };

ArtifactFailureStage artifactFailureStage = ArtifactFailureStage::Write;

class FailingArtifactFile final : public CurrentWorldArtifactFile
{
public:
    explicit FailingArtifactFile(const QString& targetPath)
        : file_(targetPath)
    {
    }

    void setDirectWriteFallback(bool enabled) override
    {
        file_.setDirectWriteFallback(enabled);
    }

    bool open() override
    {
        return file_.open(QIODevice::WriteOnly);
    }

    qint64 write(const char* data, qint64 byteCount) override
    {
        if (artifactFailureStage == ArtifactFailureStage::Write)
            return -1;
        return file_.write(data, byteCount);
    }

    bool commit() override
    {
        if (artifactFailureStage == ArtifactFailureStage::Commit) {
            file_.cancelWriting();
            return false;
        }
        return file_.commit();
    }

private:
    QSaveFile file_;
};

std::unique_ptr<CurrentWorldArtifactFile> makeFailingArtifactFile(
    const QString& targetPath)
{
    return std::make_unique<FailingArtifactFile>(targetPath);
}

struct ArtifactFileFactoryReset
{
    ~ArtifactFileFactoryReset()
    {
        setCurrentWorldArtifactFileFactoryForTesting(nullptr);
    }
};

}

TEST(CurrentWorldProjectAdapter, BusIdentityIndexIgnoresHandleValueAndSiblingOrder)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto master = project.busses.front().rootItem;
    ASSERT_NE(master, nullptr);

    constexpr std::uint32_t kMatchingHandle = 71;
    constexpr std::uint32_t kDistinctHandle = 72;
    constexpr harkenstudio::ObjectId kMatchingId{0, kMatchingHandle};
    constexpr harkenstudio::ObjectId kDistinctId{
        UINT64_C(0x3da91a64b9474560), UINT64_C(0x100000048)};
    auto matching = makeItem(QStringLiteral("Matching"), ProjectItem::Type::Bus,
                             kMatchingHandle, kMatchingId);
    auto distinct = makeItem(QStringLiteral("Distinct"), ProjectItem::Type::Bus,
                             kDistinctHandle, kDistinctId);
    ASSERT_EQ(matching->runtimeId(), kMatchingId);
    ASSERT_EQ(distinct->runtimeId(), kDistinctId);
    ASSERT_NE(kDistinctId, harkenstudio::ObjectId(0, kDistinctHandle));
    appendChild(master, matching);
    appendChild(master, distinct);

    const auto verify = [&](std::uint32_t handle,
                            harkenstudio::ObjectId expectedId) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_NE(request->identityIndex, nullptr);
        const auto* byHandle = request->identityIndex->findBySourceHandle(
            harkenprojectschema::RuntimeIdentityKindV1::Bus, handle);
        const auto* byStableId = request->identityIndex->find(expectedId);
        ASSERT_NE(byHandle, nullptr);
        ASSERT_NE(byStableId, nullptr);
        EXPECT_EQ(byHandle, byStableId);
        EXPECT_EQ(byHandle->stableId, expectedId);
        EXPECT_EQ(byHandle->authoringGuid, expectedId);
        ASSERT_NE(request->busResult, nullptr);
        EXPECT_NE(std::find(request->busResult->idSet.ids.begin(),
                            request->busResult->idSet.ids.end(), expectedId),
                  request->busResult->idSet.ids.end());
    };
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);

    std::swap(master->children[0], master->children[1]);
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);
}

TEST(CurrentWorldProjectAdapter, SelectionHistoryAndOccurrencesSurviveAuthoringBeyondDefaultCapacity)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(&container, &sound);
    auto* selection = container->comp<VSelection>();
    selection->selections.shared.find(0)->weight = 1;
    for (unsigned index = 2; index < 65; ++index)
        ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
    selection->behavior.avoidRepeat = 64;
    const auto json = ContainerBehaviorSemantics::toJson(selection->behavior);
    const auto restored = ContainerBehaviorSemantics::fromJson(json);
    ASSERT_TRUE(restored);
    selection->behavior = *restored;
    EXPECT_EQ(64, ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection, std::nullopt));
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> revisions;
    for (const auto& child : container->children) revisions.emplace(child->runtimeId(), 100 + child->objectID);
    CurrentWorldBuildOptions options;
    options.soundContentRevisions = &revisions;
    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error, nullptr, &options, 1);
    ASSERT_NE(nullptr, request) << error.toStdString();
    ASSERT_EQ(1u, request->containers.size());
    EXPECT_EQ(65u, request->containers[0].children.size());
    EXPECT_EQ(64u, request->containers[0].avoidLastN);
    EXPECT_TRUE(HarkenEditor::Bridge::compileCurrentWorldRequest(*request, true).ok());
}

TEST(CurrentWorldProjectAdapter, ScalarUpdatesMatchFullCompilationAcrossPlatformsAndRestoration)
{
    namespace Bridge = HarkenEditor::Bridge;
    using Authoring = PlayablePropertyAuthoringV1;
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(&container, &sound);
    const PlatformScope scope = HarkenTest::PlatformIds::Windows;
    const auto soundLocator = Authoring::definitionLocator(*sound);
    const auto parentLocator = Authoring::definitionLocator(*container);
    ASSERT_TRUE(soundLocator);
    ASSERT_TRUE(parentLocator);
    const auto volume = static_cast<std::uint8_t>(Property::Volume);
    const auto pitch = static_cast<std::uint8_t>(Property::Pitch);
    const auto gain = static_cast<std::uint8_t>(Property::MakeUpGain);
    const auto original = Authoring::readProperty(*sound, *soundLocator, volume, std::nullopt);
    ASSERT_TRUE(original);
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> revisions;
    for (const auto& child : container->children)
        revisions.emplace(child->runtimeId(), 100 + child->objectID);
    CurrentWorldBuildOptions options;
    options.soundContentRevisions = &revisions;
    Bridge::CurrentWorldSyncToken token{17, 1, 3, 1};
    QString error;
    auto baseline = buildCurrentWorldSyncRequest(project, scope, &error, nullptr, &options, 1);
    ASSERT_NE(baseline, nullptr) << error.toStdString();
    auto preparation = std::make_shared<Bridge::CurrentWorldSoundPreparation>();
    preparation->contentRevisions = revisions;
    Bridge::CurrentWorldSoundPreparation::Sound preparedSound;
    preparedSound.binding.soundId = sound->runtimeId();
    preparedSound.binding.sourceId = sound->sourceID;
    preparedSound.sourcePath = QStringLiteral("prepared.wav");
    preparedSound.samples = {0.125f, 0.25f};
    preparedSound.channels = 1;
    preparedSound.sampleRate = 48000;
    preparation->sounds.push_back(preparedSound);
    baseline->soundPreparation = preparation;
    Bridge::CurrentWorldRequestCache cache;
    auto cached = cache.update(baseline, {}, token);
    ASSERT_NE(cached, nullptr);
    auto first = Bridge::compileCurrentWorldRequest(*cached, true);
    ASSERT_TRUE(first.ok()) << static_cast<int>(first.status);
    const auto originalBody = first.body;
    auto wrongReceipt = token;
    ++wrongReceipt.runtimeEpoch;
    cache.acknowledgePublication(wrongReceipt);
    cached = cache.update({}, {}, token);
    EXPECT_EQ(cached->soundPreparation, preparation);
    cache.acknowledgePublication(token);
    cached = cache.update({}, {}, token);
    ASSERT_NE(cached, nullptr);
    const auto retainedPreparation = cached->soundPreparation;
    ASSERT_NE(retainedPreparation, nullptr);
    EXPECT_NE(retainedPreparation, preparation);
    ASSERT_EQ(retainedPreparation->sounds.size(), 1u);
    EXPECT_TRUE(retainedPreparation->sounds.front().samples.empty());
    EXPECT_EQ(retainedPreparation->sounds.front().binding.soundId, sound->runtimeId());
    EXPECT_EQ(retainedPreparation->sounds.front().binding.sourceId, sound->sourceID);
    EXPECT_EQ(retainedPreparation->sounds.front().sourcePath, preparedSound.sourcePath);
    EXPECT_EQ(retainedPreparation->sounds.front().channels, preparedSound.channels);
    EXPECT_EQ(retainedPreparation->sounds.front().sampleRate, preparedSound.sampleRate);
    EXPECT_EQ(retainedPreparation->contentRevisions, revisions);
    EXPECT_EQ(baseline->soundPreparation->sounds.front().samples, preparedSound.samples);

    const auto verify = [&](std::vector<std::pair<ProjectItem*, std::uint8_t>> fields) {
        std::vector<Bridge::CurrentWorldScalarUpdate> updates;
        for (const auto& [item, property] : fields) {
            Bridge::CurrentWorldScalarUpdate update;
            ASSERT_TRUE(readCurrentWorldScalarUpdate(*item, scope, property, &update));
            updates.push_back(update);
        }
        ++token.authoringRevision;
        ++token.submissionSerial;
        cached = cache.update({}, updates, token);
        ASSERT_NE(cached, nullptr);
        auto fresh = buildCurrentWorldSyncRequest(project, scope, &error, nullptr,
                                                  &options, token.authoringRevision);
        ASSERT_NE(fresh, nullptr) << error.toStdString();
        auto actual = Bridge::compileCurrentWorldRequest(*cached, true);
        auto expected = Bridge::compileCurrentWorldRequest(*fresh, true);
        ASSERT_TRUE(actual.ok()) << static_cast<int>(actual.status);
        ASSERT_TRUE(expected.ok()) << static_cast<int>(expected.status);
        EXPECT_EQ(actual.body, expected.body);
        EXPECT_EQ(actual.projection.fingerprint, expected.projection.fingerprint);
        EXPECT_EQ(actual.projection.projectGeneration, token.authoringRevision);
        EXPECT_EQ(cached->busResult->deterministicBytes, fresh->busResult->deterministicBytes);
        EXPECT_EQ(cached->soundPreparation, retainedPreparation);
    };

    ASSERT_TRUE(Authoring::setOverride(*sound, *soundLocator, volume, -6.0, std::nullopt));
    ASSERT_TRUE(Authoring::setOverride(*container, *parentLocator, gain, -3.0, std::nullopt));
    ASSERT_TRUE(Authoring::setOverride(*container, *parentLocator, pitch, 200.0, scope));
    verify({{sound.get(), volume}, {container.get(), gain}, {container.get(), pitch}});
    ASSERT_TRUE(Authoring::setOverride(*sound, *soundLocator, volume, -12.0,
                                     HarkenTest::PlatformIds::Android));
    verify({{sound.get(), volume}});
    ASSERT_TRUE(Authoring::setOverride(*sound, *soundLocator, volume, -18.0, scope));
    verify({{sound.get(), volume}});
    ASSERT_TRUE(Authoring::clearOverride(*sound, *soundLocator, volume, scope));
    verify({{sound.get(), volume}});
    ASSERT_TRUE(Authoring::restoreProperty(*sound, *soundLocator, volume, *original, std::nullopt));
    ASSERT_TRUE(Authoring::clearOverride(*container, *parentLocator, gain, std::nullopt));
    ASSERT_TRUE(Authoring::clearOverride(*container, *parentLocator, pitch, scope));
    verify({{sound.get(), volume}, {container.get(), gain}, {container.get(), pitch}});
    const auto restored = Bridge::compileCurrentWorldRequest(*cached, true);
    EXPECT_EQ(restored.body, originalBody);
    EXPECT_EQ(baseline->busResult->projectGeneration, 1u);
    EXPECT_EQ(Bridge::compileCurrentWorldRequest(*baseline, true).body, originalBody);
    const auto replacement = cache.update(baseline, {}, token);
    ASSERT_NE(replacement, nullptr);
    auto oldReceipt = token;
    --oldReceipt.submissionSerial;
    cache.acknowledgePublication(oldReceipt);
    const auto afterOldReceipt = cache.update({}, {}, token);
    ASSERT_NE(afterOldReceipt, nullptr);
    EXPECT_EQ(afterOldReceipt->soundPreparation, preparation);
    EXPECT_EQ(afterOldReceipt->soundPreparation->sounds.front().samples, preparedSound.samples);
}

TEST(CurrentWorldProjectAdapter, CachedInputsRejectForeignIdentityAndRequireReplacementAfterRestart)
{
    namespace Bridge = HarkenEditor::Bridge;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(&container, &sound);
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt);
    ASSERT_NE(request, nullptr);
    Bridge::CurrentWorldRequestCache cache;
    const Bridge::CurrentWorldSyncToken initial{1, 1, 1, 1};
    ASSERT_NE(cache.update(request, {}, initial), nullptr);
    Bridge::CurrentWorldScalarUpdate update;
    ASSERT_TRUE(readCurrentWorldScalarUpdate(*sound, std::nullopt, 1, &update));
    update.guid = {987, 654};
    EXPECT_EQ(cache.update({}, {update}, {1, 2, 1, 2}), nullptr);
    EXPECT_EQ(cache.update({}, {}, {2, 2, 1, 2}), nullptr);
    EXPECT_EQ(cache.update({}, {}, {1, 2, 2, 2}), nullptr);
    EXPECT_NE(cache.update(request, {}, {2, 2, 2, 2}), nullptr);
}

TEST(CurrentWorldArtifactWriter,
     WriteAndCommitFailuresPreserveExistingTargetWithoutDirectFallback)
{
    harkenprojectschema::ProjectInput input;
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(input);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, {}, {}, {});
    ASSERT_TRUE(compiled.ok());
    ASSERT_NE(compiled.world, nullptr);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.vnworld"));
    const QByteArray original("existing-current-world");
    QFile initial(path);
    ASSERT_TRUE(initial.open(QIODevice::WriteOnly));
    ASSERT_EQ(initial.write(original), original.size());
    initial.close();

    setCurrentWorldArtifactFileFactoryForTesting(&makeFailingArtifactFile);
    ArtifactFileFactoryReset reset;
    for (const ArtifactFailureStage stage : {
             ArtifactFailureStage::Write,
             ArtifactFailureStage::Commit}) {
        artifactFailureStage = stage;
        QString error;
        EXPECT_FALSE(WriteCurrentWorldArtifactV1(
            path, compiled.world->View(), &error));
        QFile preserved(path);
        ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
        EXPECT_EQ(preserved.readAll(), original);
    }
}

TEST(CurrentWorldArtifactWriter,
     CompilesStudioProjectIntoDecodableActionArtifact)
{
    ProjectData project = HarkenTest::projectWithPlatforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")});
    project.nextObjectID = 5;
    auto master = makeItem(QStringLiteral("Master"), ProjectItem::Type::Bus,
                           4, harkentest::StudioObjectId(0x4d4153544552));
    ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));
    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer,
                          1, harkentest::StudioObjectId(2));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 2, harkentest::StudioObjectId(3));
    appendChild(layer, variable);
    auto action = makeItem(QStringLiteral("SetState"),
                           ProjectItem::Type::Action, 3, harkentest::StudioObjectId(4));
    VActionStep setValue;
    setValue.type = VActionStep::Type::SetVariable;
    setValue.targetObjectID = variable->objectID;
    setValue.value = 42.0f;
    action->comp<VAction>()->steps.push_back(setValue);
    appendChild(layer, action);
    ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString requestedPath = qEnvironmentVariable(
        "HARKEN_TEST_CURRENT_WORLD_ARTIFACT_OUTPUT");
    const QString path = requestedPath.isEmpty()
        ? directory.filePath(QStringLiteral("project.vnworld"))
        : requestedPath;
    QString error;
    ASSERT_TRUE(WriteCurrentWorldArtifactV1(project, std::nullopt, path, &error))
        << error.toStdString();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    harken::CurrentWorldArtifactHeaderV1 header;
    ASSERT_EQ(harken::InspectCurrentWorldArtifactHeaderV1(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::size_t>(bytes.size()),
                  static_cast<std::uint64_t>(bytes.size()), header),
              harken::CurrentWorldLoadStatusV1::Ok);
    harken::CurrentWorldBodyImage image;
    ASSERT_EQ(harken::DecodeCurrentWorldArtifactBodyV1(
                  header,
                  reinterpret_cast<const std::uint8_t*>(bytes.constData())
                      + harken::kCurrentWorldArtifactHeaderBytes,
                  static_cast<std::size_t>(bytes.size())
                      - harken::kCurrentWorldArtifactHeaderBytes,
                  image),
              harken::CurrentWorldLoadStatusV1::Ok);
    const auto& world = *image.ProductionView();
    ASSERT_EQ(world.variableCount, 1u);
    EXPECT_EQ(world.variables[0].variableId, variable->runtimeId());
    ASSERT_EQ(world.actionCount, 1u);
    ASSERT_EQ(world.actions[0].stepCount, 1u);
}

}
