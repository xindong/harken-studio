#include "UnitTest/StudioWorldTestSupport.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include <harken_project_schema/FrontEnd.h>
#include <harken/core/CurrentWorldArtifact.h>
#include <harken/core/CurrentWorldCodec.h>

#include "CurrentWorldProjectAdapter.h"
#include "CurrentWorldArtifactWriter.h"
#include "CurrentWorldArtifactWriterInternal.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "ContainerBehaviorSemantics.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "RuntimeCapabilityMatrix.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "Components/VVariable.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include "Support/ProjectPlatformTestUtils.h"

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

void expectRequestCompiles(HarkenEditor::Bridge::CurrentWorldSyncRequest& request)
{
    for (auto& row : request.materializations) {
        if (row.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
            row.contentRevision = std::hash<harkenstudio::ObjectId>{}(row.stableId) + 1000;
    }
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request.project);
    std::string diagnostics;
    for (const auto& diagnostic : frontEnd.diagnostics) {
        diagnostics += harkenprojectschema::DiagnosticCodeName(diagnostic.code);
        diagnostics += " detail=" + std::to_string(diagnostic.detail);
        diagnostics += " primary=" + std::to_string(diagnostic.primary.high);
        diagnostics += ":" + std::to_string(diagnostic.primary.low);
        diagnostics += " secondary=" + std::to_string(diagnostic.secondary.high);
        diagnostics += ":" + std::to_string(diagnostic.secondary.low) + "\n";
    }
    ASSERT_TRUE(frontEnd.ok()) << diagnostics;
    ASSERT_TRUE(frontEnd.project.has_value());
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request.variables, request.materializations,
        request.containers, {}, request.transitions,
        request.actions);
    EXPECT_TRUE(compiled.ok())
        << static_cast<std::uint32_t>(compiled.status) << " related="
        << compiled.relatedStableId;
}

enum class ArtifactFailureStage { Write, Commit };

ArtifactFailureStage artifactFailureStage = ArtifactFailureStage::Write;
bool artifactDirectWriteFallback = true;

class FailingArtifactFile final : public CurrentWorldArtifactFile
{
public:
    explicit FailingArtifactFile(const QString& targetPath)
        : file_(targetPath)
    {
    }

    void setDirectWriteFallback(bool enabled) override
    {
        artifactDirectWriteFallback = enabled;
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

} // namespace

TEST(CurrentWorldProjectAdapter, ExplicitLanguageChoosesItsSourceWithoutChangingUnlocalizedSound)
{
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(nullptr, &first);
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    auto* sound = first->comp<VSound>();
    sound->distinguishLanguage = true;
    auto& english = sound->sources.localized["English"];
    english.entries = {{QUuid::createUuid(), 101}};
    english.active = english.entries[0].entryId;
    auto& japanese = sound->sources.localized["Japanese"];
    japanese.entries = {{QUuid::createUuid(), 202}};
    japanese.active = japanese.entries[0].entryId;

    CurrentWorldBuildOptions options;
    options.language = QStringLiteral("Japanese");
    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->soundSources.size(), 2u);
    EXPECT_EQ(request->soundSources[0].sourceId, 202u);
    EXPECT_EQ(request->soundSources[1].sourceId, 102u);
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter, EmptyLanguageGroupSkipsItsActionReferencesAndKeepsPlayableSiblings)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    auto* sound = first->comp<VSound>();
    sound->distinguishLanguage = true;
    auto& english = sound->sources.localized["English"];
    english.entries = {{QUuid::createUuid(), 101}};
    english.active = english.entries[0].entryId;
    const auto second = container->children.back();
    auto action = makeItem(QStringLiteral("Mixed"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    appendChild(project.layers.front().rootItem, action);
    auto voiceOnly = makeItem(QStringLiteral("VoiceOnly"), ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    appendChild(project.layers.front().rootItem, voiceOnly);
    VActionStep voiceStep;
    voiceStep.type = VActionStep::Type::Play;
    voiceStep.targetObjectID = first->objectID;
    voiceOnly->comp<VAction>()->steps.push_back(voiceStep);
    auto postOnly = makeItem(QStringLiteral("PostVoice"), ProjectItem::Type::Action, 8, harkentest::StudioObjectId(8));
    appendChild(project.layers.front().rootItem, postOnly);
    VActionStep postStep;
    postStep.type = VActionStep::Type::PostAction;
    postStep.targetObjectID = voiceOnly->objectID;
    postOnly->comp<VAction>()->steps.push_back(postStep);
    project.nextObjectID = 9;
    auto& steps = action->comp<VAction>()->steps;
    steps.resize(4);
    steps[0].type = VActionStep::Type::Play;
    steps[0].targetObjectID = first->objectID;
    steps[1].type = VActionStep::Type::Play;
    steps[1].targetObjectID = second->objectID;
    steps[1].quantize.timing = QuantizeTiming::NextMarker;
    steps[1].quantize.referenceObjectID = first->objectID;
    steps[2].type = VActionStep::Type::Play;
    steps[2].targetObjectID = second->objectID;
    steps[3].type = VActionStep::Type::PostAction;
    steps[3].targetObjectID = postOnly->objectID;

    CurrentWorldBuildOptions options;
    options.language = QStringLiteral("Japanese");
    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->soundSources.size(), 1u);
    EXPECT_EQ(request->soundSources.front().sourceId, 102u);
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->containers.front().children.size(), 1u);
    ASSERT_EQ(request->actions.size(), 1u);
    ASSERT_EQ(request->actions.front().steps.size(), 1u);
    EXPECT_EQ(request->actions.front().steps.front().target.stableId,
              second->runtimeId());
    expectRequestCompiles(*request);

    options.actionRoot = action.get();
    const auto scoped = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error, nullptr, &options);
    ASSERT_NE(scoped, nullptr) << error.toStdString();
    ASSERT_EQ(scoped->actions.size(), 1u);
    ASSERT_EQ(scoped->actions.front().steps.size(), 1u);
    EXPECT_EQ(scoped->actions.front().steps.front().target.stableId, second->runtimeId());
    ASSERT_EQ(scoped->soundSources.size(), 1u);
    EXPECT_EQ(scoped->soundSources.front().sourceId, 102u);
    expectRequestCompiles(*scoped);
}

TEST(CurrentWorldProjectAdapter, ExplicitDefinitionScopeExcludesUnrelatedMediaAndIncludesAncestors)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    auto* sound = first->comp<VSound>();
    sound->distinguishLanguage = true;
    const auto second = container->children.back();
    std::set<harkenstudio::ObjectId> definitions{second->runtimeId()};
    CurrentWorldBuildOptions options;
    options.language = QString();
    options.includedDefinitionIds = &definitions;
    QString error;
    auto request = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->soundSources.size(), 1u);
    EXPECT_EQ(request->soundSources.front().sourceId, 102u);
    EXPECT_EQ(request->project.nodes.size(), 3u);
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->containers.front().children.size(), 1u);
    expectRequestCompiles(*request);

    definitions = {first->runtimeId()};
    request = buildCurrentWorldSyncRequest(
        project, HarkenTest::PlatformIds::Windows, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->soundSources.size(), 1u);
    EXPECT_EQ(request->soundSources.front().stableId, first->runtimeId());
    EXPECT_EQ(request->soundSources.front().sourceId, 0u);
}

TEST(CurrentWorldProjectAdapter, PlatformSelectionsLowerOrderWeightsAndDuplicates)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    auto project = makeRandomProject(&container, &first);
    auto* selection = container->comp<VSelection>();
    auto& android = selection->selections.editTable(HarkenTest::PlatformIds::Android);
    android.entries.erase(android.entries.begin());
    android.entries.front().occurrences.front().weight = 23;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, android, android.entries.front().childGuid));
    auto androidRows = android.occurrenceRows();
    androidRows.back().weight = 77;
    std::reverse(androidRows.begin(), androidRows.end());
    ASSERT_TRUE(android.setOccurrenceRows(androidRows));
    selection->behavior.avoidRepeat = 5;
    for (auto platform : {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}) {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, platform, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_EQ(request->containers.size(), 1u);
        const auto& lowered = request->containers.front();
        ASSERT_EQ(lowered.children.size(), 2u);
        EXPECT_EQ(lowered.avoidLastN, platform == HarkenTest::PlatformIds::Android ? 1u : 0u);
        const auto rows = selection->activeRows(platform);
        for (size_t i = 0; i < rows.size(); ++i) {
            EXPECT_EQ(lowered.children[i].stableOccurrenceOrdinal, rows[i].stableOccurrenceOrdinal);
            EXPECT_EQ(lowered.children[i].frequencyWeight, static_cast<uint32_t>(rows[i].weight));
        }
        expectRequestCompiles(*request);
    }
    EXPECT_EQ(selection->behavior.avoidRepeat, 5);
}

TEST(CurrentWorldProjectAdapter,
     RepeatGapUsesSupportedCapacityWithoutRewritingAuthoredBehavior)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    auto project = makeRandomProject(&container, &first);
    auto* selection = container->comp<VSelection>();
    while (selection->selections.shared.occurrenceRows().size() < 10) {
        ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
            *container, first->guid));
    }
    for (auto& member : selection->selections.shared.entries)
        for (auto& row : member.occurrences)
            row.weight = 50;
    selection->behavior.avoidRepeat = 9;

    for (const auto& [playType, discipline] : {
             std::pair{SelectionPlayType::Single, RandomDiscipline::Draw},
             std::pair{SelectionPlayType::Single, RandomDiscipline::Shuffle},
             std::pair{SelectionPlayType::Queue, RandomDiscipline::Draw}}) {
        selection->mode = SelectionMode::Random;
        selection->playType = playType;
        selection->behavior.discipline = discipline;
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_EQ(request->containers.size(), 1u);
        EXPECT_EQ(request->containers.front().avoidLastN, 8u);
        expectRequestCompiles(*request);
        EXPECT_EQ(selection->behavior.avoidRepeat, 9);
    }
}

TEST(CurrentWorldProjectAdapter,
     PreservesRandomSelectionAndStableChildOccurrenceIdentity)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    const auto firstLocator =
        PlayablePropertyAuthoringV1::containerChildLocator(
            *container, first->guid);
    ASSERT_TRUE(firstLocator.has_value());

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->soundSources.size(), 2u);
    EXPECT_EQ(request->soundSources[0].stableId, first->runtimeId());
    EXPECT_EQ(request->soundSources[0].sourceId, 101u);
    const auto& selection = request->containers.front();
    EXPECT_EQ(selection.mode, harkenstudiobuild::ContainerModeV1::Random);
    EXPECT_EQ(selection.randomDiscipline, harkenstudiobuild::RandomDisciplineV1::Draw);
    EXPECT_EQ(selection.avoidLastN, 0u);
    ASSERT_EQ(selection.children.size(), 2u);
    EXPECT_EQ(selection.children[0].stableOccurrenceOrdinal,
              firstLocator->stableOccurrenceOrdinal);
    EXPECT_EQ(selection.children[0].frequencyWeight, 0u);
    EXPECT_EQ(selection.children[1].frequencyWeight, 3u);

    const auto containerNode = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == container->runtimeId();
        });
    ASSERT_NE(containerNode, request->project.nodes.end());
    ASSERT_EQ(containerNode->ownedChildren.size(), 2u);
    ASSERT_EQ(containerNode->containerChildOccurrences.size(), 2u);
    EXPECT_EQ(containerNode->containerChildOccurrences[0].stableOccurrenceOrdinal,
              firstLocator->stableOccurrenceOrdinal);
    EXPECT_EQ(containerNode->containerChildOccurrences[0].presence.bits, 0u);
    EXPECT_FLOAT_EQ(containerNode->containerChildOccurrences[0].volume, 0.0f);
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     CompilesRepeatedChildIdentitiesWithoutLegacyPlaybackOverrides)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid, std::nullopt, &duplicate));
    const auto firstLocator =
        PlayablePropertyAuthoringV1::containerChildLocator(
            *container, first->guid);
    const auto duplicateLocator =
        PlayablePropertyAuthoringV1::containerChildLocator(
            *container, duplicate.stableOccurrenceOrdinal);
    ASSERT_TRUE(firstLocator.has_value());
    ASSERT_TRUE(duplicateLocator.has_value());
    ASSERT_NE(firstLocator->stableOccurrenceOrdinal,
              duplicateLocator->stableOccurrenceOrdinal);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->containers.front().children.size(), 3u);
    EXPECT_EQ(request->containers.front().children[0].stableOccurrenceOrdinal,
              firstLocator->stableOccurrenceOrdinal);
    EXPECT_EQ(request->containers.front().children[2].stableOccurrenceOrdinal,
              duplicate.stableOccurrenceOrdinal);

    const auto containerNode = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == container->runtimeId();
        });
    ASSERT_NE(containerNode, request->project.nodes.end());
    ASSERT_EQ(containerNode->ownedChildren.size(), 2u);
    ASSERT_EQ(containerNode->containerChildOccurrences.size(), 3u);
    int matchingRows = 0;
    for (const auto& row : containerNode->containerChildOccurrences) {
        if (row.stableOccurrenceOrdinal == firstLocator->stableOccurrenceOrdinal
            || row.stableOccurrenceOrdinal == duplicate.stableOccurrenceOrdinal) {
            ++matchingRows;
            EXPECT_EQ(row.presence.bits, 0u);
            EXPECT_FLOAT_EQ(row.volume, 0.0f);
            EXPECT_FLOAT_EQ(row.pitch, 0.0f);
            EXPECT_FLOAT_EQ(row.makeUpGain, 0.0f);
            EXPECT_EQ(row.loopCount, 1u);
            EXPECT_EQ(row.streamingMode, 0u);
        }
    }
    EXPECT_EQ(matchingRows, 2);
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     LowersMatchAndBlendWithIndependentVariableResponses)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    appendChild(layer, variable);
    project.nextObjectID = 7;
    auto* definition = variable->comp<VVariable>();
    ASSERT_NE(definition, nullptr);
    definition->kind = VariableKind::Smooth;
    definition->rangeMin = 0.0f;
    definition->rangeMax = 8.0f;
    definition->defaultValue = 3.0f;

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    selection->mode = SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(first->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 3));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->matchResponse =
        VariableResponse::Live;

    QString error;
    const auto matched = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(matched, nullptr) << error.toStdString();
    ASSERT_EQ(matched->containers.size(), 1u);
    const auto& match = matched->containers.front();
    ASSERT_EQ(match.children.size(), 1u);
    ASSERT_EQ(match.discreteRows.size(), 1u);
    EXPECT_EQ(match.discreteRows.front().childStableOrdinal,
              match.children.front().stableOccurrenceOrdinal);
    EXPECT_NE(match.discreteTiming, 0u);
    EXPECT_TRUE(match.continuousRows.empty());
    expectRequestCompiles(*matched);
    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(SelectionMode::Blend)));
    const auto second = container->children[1];
    auto mixing = selection->mixing();
    mixing.rows.push_back({variable->guid, {}});
    mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(second->guid, 1.0f, 7.0f));
    mixing.rows.front().children[0].normalize();
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));

    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    const auto& lowered = request->containers.front();
    ASSERT_EQ(lowered.children.size(), 1u);
    EXPECT_NE(lowered.continuousTiming, 0u);
    EXPECT_NE(lowered.evaluationPeriodFrames, 0u);
    EXPECT_TRUE(lowered.discreteRows.empty());
    ASSERT_FALSE(lowered.continuousRows.empty());
    EXPECT_FLOAT_EQ(lowered.continuousRows[0].cells[0].lower, 1.0f);
    EXPECT_FLOAT_EQ(lowered.continuousRows[0].cells[0].upper, 7.0f);
    expectRequestCompiles(*request);
    selection->blendResponse = VariableResponse::AtStart;
    const auto snapshot = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(snapshot, nullptr) << error.toStdString();
    EXPECT_EQ(snapshot->containers[0].continuousTiming, 0u);
    EXPECT_EQ(snapshot->containers[0].evaluationPeriodFrames, 0u);
    expectRequestCompiles(*snapshot);
    auto secondVariable = makeItem(QStringLiteral("Intensity"), ProjectItem::Type::Variable,
        7, harkentest::StudioObjectId(7));
    appendChild(layer, secondVariable);
    mixing.rows.push_back({secondVariable->guid,
        {MixingChildDef::unity(second->guid, 0.0f, 100.0f)}});
    for (auto& row : mixing.rows) {
        row.children[0].points = {{0.0f, 0.5f}, {100.0f, 0.5f}};
        row.children[0].handles.clear();
        row.children[0].normalize();
    }
    const auto unboundChild = mixing.rows.front().children.front();
    mixing.rows.insert(mixing.rows.begin(), {{}, {unboundChild}});
    mixing.rows.insert(mixing.rows.begin() + 2, {{}, {unboundChild}});
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto shared = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(shared, nullptr) << error.toStdString();
    ASSERT_EQ(shared->containers[0].children.size(), 1u);
    ASSERT_EQ(shared->containers[0].continuousRows.size(), 1u);
    ASSERT_EQ(shared->containers[0].continuousRows[0].cells.size(), 2u);
    EXPECT_EQ(shared->containers[0].maxActiveRows, 1u);
    float product = 1.0f;
    for (const auto& curve : shared->containers[0].curves) product *= curve.curve.Eval(3.0f);
    EXPECT_FLOAT_EQ(product, 0.25f);
    expectRequestCompiles(*shared);
    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(SelectionMode::Match)));
    const auto restored = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(restored, nullptr) << error.toStdString();
    ASSERT_EQ(restored->containers.size(), 1u);
    EXPECT_EQ(restored->containers.front().discreteRows.front().stableRowOrdinal,
              match.discreteRows.front().stableRowOrdinal);
    EXPECT_EQ(restored->containers.front().discreteTiming, match.discreteTiming);
    EXPECT_TRUE(restored->containers.front().continuousRows.empty());
    EXPECT_EQ(selection->selections.shared.occurrenceRows().size(), 3u);
    expectRequestCompiles(*restored);
}

TEST(CurrentWorldProjectAdapter, PlatformMatchConditionsChooseUniqueActiveMembers)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    auto project = makeRandomProject(&container, &first);
    const auto second = container->children[1];
    auto variable = makeItem(QStringLiteral("State"), ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    variable->comp<VVariable>()->rangeMax = 2;
    appendChild(project.layers.front().rootItem, variable);
    project.nextObjectID = 7;
    auto* selection = container->comp<VSelection>();
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, first->guid));
    selection->mode = SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(first->guid));
        ASSERT_TRUE(grid.addRow(second->guid));
        ASSERT_TRUE(grid.addRow(first->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(grid.setCell(1, 0, 1));
        ASSERT_TRUE(grid.setCell(2, 0, 2));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->selections.editTable(HarkenTest::PlatformIds::Windows).member(second->guid)->conditions.clear();
    selection->selections.editTable(HarkenTest::PlatformIds::Android).member(first->guid)->conditions.clear();
    for (auto platform : {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}) {
        const auto& table = selection->selections.table(platform);
        EXPECT_EQ(table.occurrenceRows().size(), 3u);
        const bool windows = platform == HarkenTest::PlatformIds::Windows;
        const auto* activeMember = table.member(windows ? first->guid : second->guid);
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, platform, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        ASSERT_EQ(request->containers.size(), 1u);
        const auto& lowered = request->containers.front();
        ASSERT_EQ(lowered.children.size(), 1u);
        ASSERT_EQ(lowered.discreteRows.size(), windows ? 2u : 1u);
        EXPECT_EQ(lowered.children.front().stableOccurrenceOrdinal, activeMember->stableMemberOrdinal);
        for (const auto& row : lowered.discreteRows)
            EXPECT_EQ(row.childStableOrdinal, activeMember->stableMemberOrdinal);
        if (windows)
            EXPECT_NE(lowered.discreteRows[0].stableRowOrdinal, lowered.discreteRows[1].stableRowOrdinal);
        expectRequestCompiles(*request);
    }
}

TEST(CurrentWorldProjectAdapter,
     UnboundMixingRowsSharePlaybackAndPreserveClearedVariableCurves)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    auto project = makeRandomProject(&container, &first);
    const auto second = container->children[1];
    auto variable = makeItem(QStringLiteral("Gain"), ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    variable->comp<VVariable>()->kind = VariableKind::Smooth;
    variable->comp<VVariable>()->rangeMax = 100;
    appendChild(project.layers.front().rootItem, variable);
    project.nextObjectID = 7;
    auto* selection = container->comp<VSelection>();
    selection->mode = SelectionMode::Blend;
    auto curve = MixingChildDef::unity(first->guid, 0.0f, 100.0f);
    for (auto& point : curve.points) point.value = 0.25f;
    curve.handles.clear();
    curve.normalize();
    MixingDef mixing{{{{}, {curve}}, {{}, {curve, {second->guid, {}, {}}}}}};
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto saved = selection->selections.toJson();
    const auto parsed = SelectionData::fromJson(saved);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->toJson(), saved);
    EXPECT_EQ(parsed->shared.mixing().toJson(), mixing.toJson());
    selection->selections = *parsed;
    const auto evaluated = MixingSemantics::evaluate(mixing, [](const QUuid&) -> std::optional<float> {
        ADD_FAILURE() << "Unbound mixing rows must not resolve a Variable";
        return std::nullopt;
    });
    ASSERT_TRUE(evaluated.has_value());
    EXPECT_FLOAT_EQ(evaluated->value(first->guid), 1.0f);
    EXPECT_FLOAT_EQ(evaluated->value(second->guid), 1.0f);
    QString error;
    const auto initial = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(initial, nullptr) << error.toStdString();
    ASSERT_EQ(initial->containers.size(), 1u);
    EXPECT_TRUE(initial->containers.front().continuousAxes.empty());
    EXPECT_TRUE(initial->containers.front().curves.empty());
    EXPECT_EQ(initial->containers.front().children.size(), 2u);
    EXPECT_EQ(initial->containers.front().continuousRows.size(), 2u);
    EXPECT_EQ(initial->containers.front().maxActiveRows, 2u);
    expectRequestCompiles(*initial);

    mixing.rows.push_back({variable->guid, {curve}});
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto mixed = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(mixed, nullptr) << error.toStdString();
    ASSERT_EQ(mixed->containers.front().continuousAxes.size(), 1u);
    ASSERT_EQ(mixed->containers.front().continuousRows.size(), 2u);
    ASSERT_EQ(mixed->containers.front().curves.size(), 1u);
    EXPECT_FLOAT_EQ(mixed->containers.front().curves.front().curve.Eval(50.0f), 0.25f);
    expectRequestCompiles(*mixed);

    const auto configured = mixing.toJson();
    mixing.rows.back().variable = QUuid{};
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto cleared = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(cleared, nullptr) << error.toStdString();
    EXPECT_TRUE(cleared->containers.front().continuousAxes.empty());
    EXPECT_TRUE(cleared->containers.front().curves.empty());
    EXPECT_EQ(cleared->containers.front().children.size(), 2u);
    EXPECT_EQ(cleared->containers.front().continuousRows.size(), 2u);
    expectRequestCompiles(*cleared);
    mixing = selection->mixing();
    mixing.rows.back().variable = variable->guid;
    EXPECT_EQ(mixing.toJson(), configured);
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto restored = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(restored, nullptr) << error.toStdString();
    ASSERT_EQ(restored->containers.front().curves.size(), 1u);
    EXPECT_FLOAT_EQ(restored->containers.front().curves.front().curve.Eval(50.0f), 0.25f);
    expectRequestCompiles(*restored);
}

TEST(CurrentWorldProjectAdapter,
     LowersBasicTransitionRulesAndFadeCurvesIntoTypedWorld)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    const std::shared_ptr<ProjectItem> second = container->children[1];
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.sourceFadeOut.durationSeconds = 0.01f;
    defaultRule.targetFadeIn.durationSeconds = 0.02f;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    TransitionRuleDef exactRule = canonicalTransitionRule();
    exactRule.source = first->guid;
    exactRule.target = second->guid;
    exactRule.exitTiming = TransitionExitTiming::NaturalEnd;
    exactRule.sourceFadeOut.durationSeconds = 0.03f;
    exactRule.targetFadeIn.durationSeconds = 0.04f;
    ASSERT_TRUE(selection->transitions.addRule(exactRule));

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->transitions.size(), 1u);
    EXPECT_EQ(request->containers[0].transitionProgramIndex, 0u);

    const auto& transition = request->transitions[0];
    EXPECT_EQ(transition.ownerContainerStableId, container->runtimeId());
    EXPECT_EQ(transition.defaultRuleIndex, 0u);
    ASSERT_EQ(transition.rules.size(), 2u);
    const auto& loweredDefault = transition.rules[0];
    EXPECT_EQ(loweredDefault.sourceAny, 1u);
    EXPECT_EQ(loweredDefault.targetAny, 1u);
    EXPECT_EQ(loweredDefault.sourceExit,
              harkenstudiobuild::TransitionSourceExitV1::Immediate);
    EXPECT_EQ(loweredDefault.sourceFade.durationFrames, 480u);
    EXPECT_EQ(loweredDefault.targetFade.durationFrames, 960u);

    const auto& loweredExact = transition.rules[1];
    EXPECT_EQ(loweredExact.sourceAny, 0u);
    EXPECT_EQ(loweredExact.targetAny, 0u);
    EXPECT_EQ(loweredExact.source.kind, harkenstudiobuild::PlayableKindV1::Sound);
    EXPECT_EQ(loweredExact.source.stableId, first->runtimeId());
    EXPECT_EQ(loweredExact.target.kind, harkenstudiobuild::PlayableKindV1::Sound);
    EXPECT_EQ(loweredExact.target.stableId, second->runtimeId());
    EXPECT_EQ(loweredExact.sourceExit,
              harkenstudiobuild::TransitionSourceExitV1::NaturalEnd);
    EXPECT_EQ(loweredExact.sourceFade.durationFrames, 1440u);
    EXPECT_EQ(loweredExact.targetFade.durationFrames, 1920u);

    const auto hasCurve = [&](std::uint32_t curveId) {
        return std::any_of(
            request->containers[0].curves.begin(),
            request->containers[0].curves.end(),
            [&](const harkenstudiobuild::CurveV1& curve) {
                return curve.curveLocalId == curveId;
            });
    };
    EXPECT_TRUE(hasCurve(loweredDefault.sourceFade.curveLocalId));
    EXPECT_TRUE(hasCurve(loweredDefault.targetFade.curveLocalId));
    EXPECT_TRUE(hasCurve(loweredExact.sourceFade.curveLocalId));
    EXPECT_TRUE(hasCurve(loweredExact.targetFade.curveLocalId));
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     LowersTransitionFadeSecondsAtTheProjectSampleRate)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    project.sampleRate = 44100;
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.sourceFadeOut.durationSeconds = 1.0f;
    defaultRule.targetFadeIn.durationSeconds = 0.5f;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->transitions.size(), 1u);
    ASSERT_EQ(request->transitions[0].rules.size(), 1u);
    EXPECT_EQ(request->transitions[0].rules[0].sourceFade.durationFrames,
              44100u);
    EXPECT_EQ(request->transitions[0].rules[0].targetFade.durationFrames,
              22050u);
}

TEST(CurrentWorldProjectAdapter,
     RefusesEveryMusicalSourceExitAsUnavailableV2Capability)
{
    const std::pair<TransitionExitTiming, QString> cases[] = {
        {TransitionExitTiming::NextBeat, QStringLiteral("NextBeat")},
        {TransitionExitTiming::NextBar, QStringLiteral("NextBar")},
        {TransitionExitTiming::NextMarker, QStringLiteral("NextMarker")},
        {TransitionExitTiming::ExitMarker, QStringLiteral("ExitMarker")},
    };
    for (const auto& [timing, name] : cases) {
        std::shared_ptr<ProjectItem> container;
        ProjectData project = makeRandomProject(&container, nullptr);
        auto* selection = container->comp<VNX::VSelection>();
        ASSERT_NE(selection, nullptr);
        selection->mode = SelectionMode::Sequence;
        selection->playType = SelectionPlayType::Queue;

        TransitionRuleDef defaultRule = selection->transitions.rules[0];
        defaultRule.exitTiming = timing;
        defaultRule.sourceFadeOut.durationSeconds = 0.25f;
        ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
        EXPECT_EQ(request, nullptr) << name.toStdString();
        EXPECT_EQ(error, QStringLiteral(
            "Current World Transition supports Immediate and Natural End exits; %1 is unavailable")
            .arg(name));
    }
}

TEST(CurrentWorldProjectAdapter,
     RefusesBridgeAsUnavailableV3BridgeTransitionCapability)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> bridge;
    ProjectData project = makeRandomProject(&container, &bridge);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.bridge = bridge->guid;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_EQ(error, QStringLiteral(
        "Current World Transition does not support a Bridge"));
}

TEST(CurrentWorldProjectAdapter,
     RefusesSamePositionAsUnavailableV2MusicalTransitionCapability)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.entryMode = TransitionEntryMode::SamePosition;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_EQ(error, QStringLiteral(
        "Current World Transition does not support SamePosition target entry"));
}

TEST(CurrentWorldProjectAdapter,
     PreservesAuthoredTrackClipDurationIntoCurrentWorldSchedule)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    ASSERT_EQ(container->children.size(), 2u);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto track = makeItem(QStringLiteral("Duration Track"),
                          ProjectItem::Type::Track, 4,
                          harkentest::StudioObjectId(5));
    track->parent = container.get();
    container->children[1] = track;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, track->guid));
    auto trackSound = makeItem(QStringLiteral("Duration Sound"),
                               ProjectItem::Type::Sound, 7,
                               harkentest::StudioObjectId(7));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources = sound->comp<VSound>()->sources;
    appendChild(track, trackSound);
    project.nextObjectID = 8;

    constexpr std::int64_t kAuthoredDuration = 1250000000;
    Clip clip;
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    clip.targetGuid = trackSound->guid;
    clip.duration = kAuthoredDuration;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    const auto node = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [](const auto& candidate) {
            return candidate.kind == harkenprojectschema::ContentKind::Track;
        });
    ASSERT_NE(node, request->project.nodes.end());
    ASSERT_EQ(node->clips.size(), 1u);
    EXPECT_EQ(node->clips.front().duration, kAuthoredDuration);

    for (auto& materialization : request->materializations) {
        if (materialization.kind
            == harkenplayableproperties::ContentPropertyKindV1::Sound) {
            materialization.contentRevision = 1;
        }
    }
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request->project);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request->variables, request->materializations,
        request->containers);
    ASSERT_TRUE(compiled.ok());
    const auto world = compiled.world->View();
    const auto schedule = std::find_if(
        world.trackSchedules, world.trackSchedules + world.trackScheduleCount,
        [&](const auto& candidate) {
            return candidate.rootDefinition.stableId == track->runtimeId();
        });
    ASSERT_NE(schedule, world.trackSchedules + world.trackScheduleCount);
    ASSERT_EQ(schedule->placementCount, 1u);
    EXPECT_EQ(schedule->placements[0].authoredDuration, kAuthoredDuration);
}

TEST(CurrentWorldProjectAdapter,
     LowersCompositeTrackAsBoundedQueueTransitionEndpoint)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    ASSERT_EQ(container->children.size(), 2u);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto track = makeItem(QStringLiteral("Composite Track"),
                          ProjectItem::Type::Track, 4, harkentest::StudioObjectId(5));
    track->parent = container.get();
    container->children[1] = track;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, track->guid));
    auto trackSound = makeItem(QStringLiteral("Track Sound"), ProjectItem::Type::Sound, 7, harkentest::StudioObjectId(7));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources = sound->comp<VSound>()->sources;
    appendChild(track, trackSound);
    project.nextObjectID = 8;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    for (std::uint32_t index = 0; index < 2; ++index) {
        Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.targetGuid = trackSound->guid;
        clip.start = index * 1000000;
        clip.duration = 1000000;
        clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10 + index));
        ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
        timeline->clips.items.push_back(clip);
    }

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.sourceFadeOut.durationSeconds = 0.01f;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));
    TransitionRuleDef exactRule = canonicalTransitionRule();
    exactRule.source = sound->guid;
    exactRule.target = track->guid;
    exactRule.exitTiming = TransitionExitTiming::Immediate;
    exactRule.targetFadeIn.durationSeconds = 0.01f;
    ASSERT_TRUE(selection->transitions.addRule(exactRule));

    QString error;
    CurrentWorldBuildStatus status =
        CurrentWorldBuildStatus::InvalidAuthoring;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, &status);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
    ASSERT_EQ(request->transitions.size(), 1u);
    ASSERT_EQ(request->transitions[0].rules.size(), 2u);
    const auto& lowered = request->transitions[0].rules[1];
    EXPECT_EQ(lowered.source.kind, harkenstudiobuild::PlayableKindV1::Sound);
    EXPECT_EQ(lowered.source.stableId, sound->runtimeId());
    EXPECT_EQ(lowered.target.kind, harkenstudiobuild::PlayableKindV1::Track);
    EXPECT_EQ(lowered.target.stableId, track->runtimeId());
    expectRequestCompiles(*request);

    const RuntimeCapabilityAssessment assessment =
        RuntimeCapabilityMatrix::assess(
            *container, project, RuntimeCapabilityOperation::LiveSync);
    EXPECT_TRUE(assessment.permitted());
}

TEST(CurrentWorldProjectAdapter,
     LowersSingleSoundTrackAsExactTransitionEndpoint)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    ASSERT_EQ(container->children.size(), 2u);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto track = makeItem(QStringLiteral("Single Sound Track"),
                          ProjectItem::Type::Track, 4, harkentest::StudioObjectId(5));
    track->parent = container.get();
    container->children[1] = track;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, track->guid));
    auto trackSound = makeItem(QStringLiteral("Track Sound"), ProjectItem::Type::Sound, 7, harkentest::StudioObjectId(7));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources = sound->comp<VSound>()->sources;
    appendChild(track, trackSound);
    project.nextObjectID = 8;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = trackSound->guid;
    clip.duration = 1000000;
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));
    TransitionRuleDef exactRule = canonicalTransitionRule();
    exactRule.source = sound->guid;
    exactRule.target = track->guid;
    exactRule.exitTiming = TransitionExitTiming::Immediate;
    exactRule.targetFadeIn.durationSeconds = 0.01f;
    ASSERT_TRUE(selection->transitions.addRule(exactRule));

    QString error;
    CurrentWorldBuildStatus status =
        CurrentWorldBuildStatus::InvalidAuthoring;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, &status);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
    ASSERT_EQ(request->transitions.size(), 1u);
    ASSERT_EQ(request->transitions[0].rules.size(), 2u);
    const auto& lowered = request->transitions[0].rules[1];
    EXPECT_EQ(lowered.source.kind, harkenstudiobuild::PlayableKindV1::Sound);
    EXPECT_EQ(lowered.source.stableId, sound->runtimeId());
    EXPECT_EQ(lowered.target.kind, harkenstudiobuild::PlayableKindV1::Track);
    EXPECT_EQ(lowered.target.stableId, track->runtimeId());
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     LowersCompositePieceAsBoundedQueueTransitionEndpoint)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    ASSERT_EQ(container->children.size(), 2u);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto piece = makeItem(QStringLiteral("Composite Piece"),
                          ProjectItem::Type::Piece, 4, harkentest::StudioObjectId(5));
    piece->parent = container.get();
    container->children[1] = piece;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, piece->guid));
    auto track = makeItem(QStringLiteral("Piece Track"),
                          ProjectItem::Type::Track, 6, harkentest::StudioObjectId(6));
    appendChild(piece, track);
    project.nextObjectID = 7;

    auto trackSound = makeItem(QStringLiteral("Track Sound"), ProjectItem::Type::Sound, 7, harkentest::StudioObjectId(7));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources = sound->comp<VSound>()->sources;
    appendChild(track, trackSound);
    project.nextObjectID = 8;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    for (std::uint32_t index = 0; index < 2; ++index) {
        Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.targetGuid = trackSound->guid;
        clip.start = index * 1000000;
        clip.duration = 1000000;
        clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(20 + index));
        ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
        timeline->clips.items.push_back(clip);
    }

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));
    TransitionRuleDef exactRule = canonicalTransitionRule();
    exactRule.source = sound->guid;
    exactRule.target = piece->guid;
    exactRule.exitTiming = TransitionExitTiming::Immediate;
    exactRule.targetFadeIn.durationSeconds = 0.01f;
    ASSERT_TRUE(selection->transitions.addRule(exactRule));

    QString error;
    CurrentWorldBuildStatus status =
        CurrentWorldBuildStatus::InvalidAuthoring;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, &status);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
    ASSERT_EQ(request->transitions.size(), 1u);
    ASSERT_EQ(request->transitions[0].rules.size(), 2u);
    const auto& lowered = request->transitions[0].rules[1];
    EXPECT_EQ(lowered.source.kind, harkenstudiobuild::PlayableKindV1::Sound);
    EXPECT_EQ(lowered.source.stableId, sound->runtimeId());
    EXPECT_EQ(lowered.target.kind, harkenstudiobuild::PlayableKindV1::Piece);
    EXPECT_EQ(lowered.target.stableId, piece->runtimeId());
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     AllowsCompositeTrackTransitionOnlyOnDiscreteVariableControlSide)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    const auto replaced = PlayablePropertyAuthoringV1::containerChildLocator(
        *container, container->children[1]->guid);
    ASSERT_TRUE(replaced.has_value());

    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    variable->parent = layer.get();
    layer->children.push_back(variable);
    auto* definition = variable->comp<VVariable>();
    ASSERT_NE(definition, nullptr);
    definition->kind = VariableKind::Step;
    definition->rangeMin = 0.0f;
    definition->rangeMax = 1.0f;

    auto track = makeItem(QStringLiteral("Single Sound Track"),
                          ProjectItem::Type::Track, 4, harkentest::StudioObjectId(5));
    track->parent = container.get();
    container->children[1] = track;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *container, replaced->stableOccurrenceOrdinal, track->guid));
    auto trackSound = makeItem(QStringLiteral("Track Sound"), ProjectItem::Type::Sound, 7, harkentest::StudioObjectId(7));
    trackSound->sourceID = sound->sourceID;
    trackSound->comp<VSound>()->sources = sound->comp<VSound>()->sources;
    appendChild(track, trackSound);
    project.nextObjectID = 8;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    for (std::uint32_t index = 0; index < 2; ++index) {
        Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.targetGuid = trackSound->guid;
        clip.start = index * 1000000;
        clip.duration = 1000000;
        clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10 + index));
        ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
        timeline->clips.items.push_back(clip);
    }

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(track->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 1));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->matchResponse =
        VariableResponse::Live;
    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::Immediate;
    defaultRule.targetFadeIn.durationSeconds = 0.01f;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    QString error;
    CurrentWorldBuildStatus status =
        CurrentWorldBuildStatus::InvalidAuthoring;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, &status);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->containers[0].discreteRows.size(), 1u);
    const auto trackLocator =
        PlayablePropertyAuthoringV1::containerChildLocator(
            *container, track->guid);
    ASSERT_TRUE(trackLocator.has_value());
    EXPECT_EQ(request->containers[0].discreteRows[0].childStableOrdinal,
              trackLocator->stableOccurrenceOrdinal);
    ASSERT_EQ(request->transitions.size(), 1u);
    expectRequestCompiles(*request);

    selection->matchResponse = VariableResponse::AtStart;
    const auto snapshot = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(snapshot, nullptr) << error.toStdString();
    EXPECT_TRUE(snapshot->transitions.empty());
    EXPECT_EQ(selection->transitions.rules[0], defaultRule);
    expectRequestCompiles(*snapshot);
    selection->matchResponse = VariableResponse::Live;

    const RuntimeCapabilityAssessment assessment =
        RuntimeCapabilityMatrix::assess(
            *container, project, RuntimeCapabilityOperation::LiveSync);
    EXPECT_TRUE(assessment.permitted());

    ASSERT_TRUE(selection->selections.shared.setMatchGrid({}));
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(sound->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->matchResponse =
        VariableResponse::Live;
    MixingDef mixing;
    mixing.rows.push_back({variable->guid, {}});
    mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(track->guid, 0.0f, 100.0f));
    mixing.rows.front().children[0].normalize();
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));

    status = CurrentWorldBuildStatus::Ready;
    error.clear();
    const auto dormant = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, &status);
    ASSERT_NE(dormant, nullptr) << error.toStdString();
    EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
    EXPECT_TRUE(dormant->containers.front().continuousRows.empty());
    ASSERT_EQ(dormant->containers.front().children.size(), 1u);
    expectRequestCompiles(*dormant);
}

TEST(RuntimeCapabilityMatrixTest,
     ClassifiesDeferredTransitionAsUnavailableCapabilityNotInvalidAuthoring)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;

    TransitionRuleDef defaultRule = selection->transitions.rules[0];
    defaultRule.exitTiming = TransitionExitTiming::NextBeat;
    ASSERT_TRUE(selection->transitions.updateRule(0, defaultRule));

    const RuntimeCapabilityAssessment assessment =
        RuntimeCapabilityMatrix::assess(
            *container, project, RuntimeCapabilityOperation::LiveSync);
    EXPECT_FALSE(assessment.permitted());
    EXPECT_EQ(assessment.state,
              RuntimeCapabilityState::UnavailableEngineContract);
    ASSERT_EQ(assessment.issues.size(), 1u);
    EXPECT_EQ(assessment.issues[0].feature,
              RuntimeCapabilityFeature::Transition);
    EXPECT_EQ(assessment.issues[0].diagnostic, QStringLiteral(
        "Current World Transition supports Immediate and Natural End exits; NextBeat is unavailable"));
}

TEST(CurrentWorldProjectAdapter,
     StreamingQueueSoundRemainsSignalOnlyAndFiniteQueueLoopCountsCycles)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    first->comp<VAudio>()->streamingMode = 1;

    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    ASSERT_EQ(request->containers[0].children.size(), 2u);
    EXPECT_EQ(request->containers[0].children[0].completion.completionClass,
              harkenstudiobuild::AuthoredCompletionClassV1::SignalOnly);
    EXPECT_TRUE(request->containers[0].children[0].completion.leaves.empty());
    EXPECT_EQ(request->containers[0].children[1].completion.completionClass,
              harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate);

    container->comp<VAudio>()->loopCount = 3;
    error.clear();
    request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers[0].queueCycle, harkenstudiobuild::QueueCycleV1::Loop);
    EXPECT_EQ(request->containers[0].queueCycleCount, 3u);
}

TEST(CurrentWorldProjectAdapter,
     EmptyContainerDoesNotBlockPlayableSequenceSibling)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;

    auto empty = makeItem(QStringLiteral("Empty"),
                          ProjectItem::Type::Container, 6, harkentest::StudioObjectId(6));
    ProjectItem* layer = container->parent;
    ASSERT_NE(layer, nullptr);
    empty->parent = layer;
    layer->children.push_back(empty);
    project.nextObjectID = 7;

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers.front().containerStableId,
              container->runtimeId());
    EXPECT_EQ(request->containers.front().mode,
              harkenstudiobuild::ContainerModeV1::Sequence);

    const auto emptyNode = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == empty->runtimeId();
        });
    EXPECT_EQ(emptyNode, request->project.nodes.end());
    EXPECT_FALSE(isCurrentWorldPlayable(project, *empty));
    EXPECT_TRUE(isCurrentWorldPlayable(project, *container));
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     EmptyVariableControlGridDoesNotBlockPlayableSequenceSibling)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Sequence;

    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    auto emptyGrid = makeItem(QStringLiteral("EmptyGrid"),
                              ProjectItem::Type::Container, 6, harkentest::StudioObjectId(6));
    auto child = makeItem(QStringLiteral("GridChild"),
                          ProjectItem::Type::Sound, 7, harkentest::StudioObjectId(7));
    appendChild(layer, emptyGrid);
    appendChild(emptyGrid, child);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *emptyGrid, child->guid));
    emptyGrid->comp<VNX::VSelection>()->mode = SelectionMode::Match;
    project.nextObjectID = 8;

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers.front().containerStableId,
              container->runtimeId());
    EXPECT_EQ(request->containers.front().mode,
              harkenstudiobuild::ContainerModeV1::Sequence);
    EXPECT_FALSE(isCurrentWorldPlayable(project, *emptyGrid));
    EXPECT_TRUE(isCurrentWorldPlayable(project, *container));
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     EmptyNestedContainerOccurrenceDoesNotBlockPlayableParent)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    auto empty = makeItem(QStringLiteral("EmptyNested"),
                          ProjectItem::Type::Container, 6, harkentest::StudioObjectId(6));
    appendChild(container, empty);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, empty->guid));
    project.nextObjectID = 7;

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers.front().containerStableId,
              container->runtimeId());
    const uint32_t emptyOrdinal =
        container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().back().stableOccurrenceOrdinal;
    ASSERT_EQ(request->containers.front().children.size(), 2u);
    for (const auto& child : request->containers.front().children)
        EXPECT_NE(child.stableOccurrenceOrdinal, emptyOrdinal);

    const auto containerNode = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == container->runtimeId();
        });
    ASSERT_NE(containerNode, request->project.nodes.end());
    EXPECT_EQ(containerNode->ownedChildren.size(), 2u);
    ASSERT_EQ(containerNode->containerChildOccurrences.size(), 2u);
    for (const auto& row : containerNode->containerChildOccurrences)
        EXPECT_NE(row.stableOccurrenceOrdinal, emptyOrdinal);

    const auto emptyNode = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == empty->runtimeId();
        });
    EXPECT_EQ(emptyNode, request->project.nodes.end());
    EXPECT_FALSE(isCurrentWorldPlayable(project, *empty));
    EXPECT_TRUE(isCurrentWorldPlayable(project, *container));
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter, EmptyCurrentWorldIsCanonical)
{
    harkenprojectschema::ProjectInput input;
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(input);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, {}, {}, {});
    ASSERT_TRUE(compiled.ok());
    ASSERT_NE(compiled.world, nullptr);
    EXPECT_EQ(harkentest::ValidateCompilerWorld(compiled.world->View()),
              harken::WorldInstallStatus::Ok);
}

TEST(CurrentWorldProjectAdapter, SharesOneRuntimeIdentityIndexWithProjectInput)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> firstSound;
    ProjectData project = makeRandomProject(&container, &firstSound);
    project.layers.front().rootItem->guid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    CurrentWorldBuildStatus status = CurrentWorldBuildStatus::InvalidAuthoring;
    QString error;
    constexpr std::uint64_t projectGeneration = 313;
    const auto request = buildCurrentWorldSyncRequest(
        project, PlatformScope{}, &error, &status, nullptr,
        projectGeneration);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_NE(request->identityIndex, nullptr);
    EXPECT_EQ(request->identityIndex.get(), request->project.identityIndex.get());
    ASSERT_NE(request->compileTargetProfile, nullptr);
    EXPECT_EQ(request->compileTargetProfile.get(),
              request->project.compileTargetProfile.get());
    ASSERT_NE(request->busResult, nullptr);
    EXPECT_EQ(request->busResult->identityIndex.get(),
              request->identityIndex.get());
    EXPECT_EQ(request->busResult->compileTargetProfile.get(),
              request->compileTargetProfile.get());
    EXPECT_EQ(request->busResult->projectGeneration, projectGeneration);
    ASSERT_FALSE(request->busResult->idSet.ids.empty());
    EXPECT_TRUE(std::is_sorted(request->busResult->idSet.ids.begin(),
                               request->busResult->idSet.ids.end()));
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
        EXPECT_TRUE(std::binary_search(request->busResult->idSet.ids.begin(),
                                       request->busResult->idSet.ids.end(), expectedId));
    };
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);

    std::swap(master->children[0], master->children[1]);
    verify(kMatchingHandle, kMatchingId);
    verify(kDistinctHandle, kDistinctId);
}

TEST(CurrentWorldProjectAdapter,
     MapsGroupGuidsToCanonicalBusIdsAndRejectsDanglingReferences)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    project.layers.front().rootItem->guid = VNX::authoringUuid(harkentest::StudioObjectId(10));

    auto master = project.busses.front().rootItem;
    ASSERT_NE(master, nullptr);
    auto child = makeItem(QStringLiteral("Child"), ProjectItem::Type::Bus,
                          900, harkentest::StudioObjectId(UINT64_C(0x100000005)));
    appendChild(master, child);

    auto* mix = sound->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    sound->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(child->guid), -3.0f}};
    sound->fieldGroups.send.shared = {
        FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(master->guid), -6.0f}}}};

    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    const auto row = std::find_if(
        request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) {
            return node.guid == sound->runtimeId();
        });
    ASSERT_NE(row, request->project.nodes.end());
    EXPECT_EQ(row->mainOutputBusRef, child->runtimeId());
    ASSERT_EQ(row->sends.size(), 1u);
    EXPECT_EQ(row->sends.front().targetBusId, master->runtimeId());
    EXPECT_FLOAT_EQ(row->sends.front().sendLevel, -6.0f);

    sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef =
        toFieldGroupGuid(QUuid::createUuid());
    error.clear();
    request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_TRUE(error.startsWith(QStringLiteral("Field group resolution failed")));
}

TEST(CurrentWorldProjectAdapter,
     LowersFiveActionOpcodesAndVariableResetScopes)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);

    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    appendChild(layer, variable);

    auto targetAction = makeItem(QStringLiteral("TargetAction"),
                                 ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    appendChild(layer, targetAction);
    VActionStep targetPlay;
    targetPlay.type = VActionStep::Type::Play;
    targetPlay.targetObjectID = sound->objectID;
    targetAction->comp<VAction>()->steps.push_back(targetPlay);
    auto action = makeItem(QStringLiteral("RootAction"),
                           ProjectItem::Type::Action, 8, harkentest::StudioObjectId(8));
    appendChild(layer, action);

    auto* authored = action->comp<VAction>();
    ASSERT_NE(authored, nullptr);
    authored->steps.resize(6);
    authored->steps[0].type = VActionStep::Type::Play;
    authored->steps[0].targetObjectID = sound->objectID;
    authored->steps[1].type = VActionStep::Type::Stop;
    authored->steps[1].targetObjectID = container->objectID;
    authored->steps[1].fadeTime = 12.5f;
    authored->steps[2].type = VActionStep::Type::PostAction;
    authored->steps[2].targetObjectID = targetAction->objectID;
    authored->steps[3].type = VActionStep::Type::SetVariable;
    authored->steps[3].targetObjectID = variable->objectID;
    authored->steps[3].value = 17.0f;
    authored->steps[4].type = VActionStep::Type::Reset;
    authored->steps[4].targetObjectID = container->objectID;
    authored->steps[4].resetScope = ResetScope::Global;
    authored->steps[5].type = VActionStep::Type::Reset;
    authored->steps[5].targetObjectID = container->objectID;
    authored->steps[5].resetScope = ResetScope::Emitter;
    for (size_t i = 3; i < authored->steps.size(); ++i) {
        authored->steps[i].quantize.timing = QuantizeTiming::EntryMarker;
        authored->steps[i].quantize.referenceObjectID = UINT32_MAX;
        authored->steps[i].quantize.ifSilent = QuantizeIfSilent::Drop;
    }

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->variables.size(), 1u);
    EXPECT_EQ(request->variables[0].variableId, variable->runtimeId());
    ASSERT_EQ(request->actions.size(), 2u);
    const auto root = std::find_if(
        request->actions.begin(), request->actions.end(),
        [&](const auto& row) {
            return row.actionStableId == action->runtimeId();
        });
    ASSERT_NE(root, request->actions.end());
    ASSERT_EQ(root->steps.size(), 6u);
    EXPECT_EQ(root->steps[0].opcode, harkenstudiobuild::ActionOpcodeV1::Play);
    EXPECT_EQ(root->steps[0].target.kind, harkenstudiobuild::ActionTargetKindV1::Sound);
    EXPECT_EQ(root->steps[1].opcode, harkenstudiobuild::ActionOpcodeV1::Stop);
    EXPECT_EQ(root->steps[1].target.kind,
              harkenstudiobuild::ActionTargetKindV1::Container);
    EXPECT_FLOAT_EQ(root->steps[1].fadeMilliseconds, 12.5f);
    EXPECT_EQ(root->steps[2].opcode, harkenstudiobuild::ActionOpcodeV1::PostAction);
    EXPECT_EQ(root->steps[2].target.kind, harkenstudiobuild::ActionTargetKindV1::Action);
    EXPECT_EQ(root->steps[3].opcode, harkenstudiobuild::ActionOpcodeV1::SetVariable);
    EXPECT_EQ(root->steps[3].target.kind, harkenstudiobuild::ActionTargetKindV1::Variable);
    EXPECT_FLOAT_EQ(root->steps[3].value, 17.0f);
    EXPECT_EQ(root->steps[4].opcode, harkenstudiobuild::ActionOpcodeV1::Reset);
    EXPECT_EQ(root->steps[4].resetScope, harkenstudiobuild::ActionScopeV1::Global);
    EXPECT_EQ(root->steps[5].opcode, harkenstudiobuild::ActionOpcodeV1::Reset);
    EXPECT_EQ(root->steps[5].resetScope, harkenstudiobuild::ActionScopeV1::Emitter);
    for (const auto& step : root->steps) {
        EXPECT_EQ(step.quantizeTiming, harkenstudiobuild::ActionQuantizeTimingV1::Immediate);
        EXPECT_EQ(step.quantizeReference.stableId, harkentest::StudioObjectId(0u));
        EXPECT_EQ(step.ifSilent, harkenstudiobuild::ActionIfSilentV1::Immediate);
        EXPECT_FLOAT_EQ(step.cooldownSeconds, 0.0f);
        EXPECT_EQ(step.cooldownScope, harkenstudiobuild::ActionCooldownScopeV1::Global);
    }
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter,
     RejectsStalePrimaryActionIdentityDespiteUniqueCompatibleName)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);

    auto wrongKind = makeItem(QStringLiteral("State"),
                              ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    appendChild(layer, wrongKind);
    auto action = makeItem(QStringLiteral("ReboundAction"),
                           ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    appendChild(layer, action);
    VActionStep play;
    play.type = VActionStep::Type::Play;
    play.targetObjectID = wrongKind->objectID;
    play.targetName = sound->name;
    action->comp<VAction>()->steps.push_back(play);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_FALSE(error.isEmpty());
}

TEST(CurrentWorldProjectAdapter, RejectsMissingVariableDefinition)
{
    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeRandomProject(&container, nullptr);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    variable->components.remove<VVariable>();
    appendChild(layer, variable);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_EQ(error, QStringLiteral("Variable definition is invalid"));
}

TEST(CurrentWorldProjectAdapter,
     ResolvesQuantizeReferencesWithExactIdAndImmediateSentinelRules)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> first;
    ProjectData project = makeRandomProject(&container, &first);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    const auto second = container->children[1];

    auto variable = makeItem(QStringLiteral("State"),
                             ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    appendChild(layer, variable);
    auto action = makeItem(QStringLiteral("QuantizedAction"),
                           ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    appendChild(layer, action);
    auto* authored = action->comp<VAction>();
    ASSERT_NE(authored, nullptr);
    authored->steps.resize(4);

    authored->steps[0].type = VActionStep::Type::Play;
    authored->steps[0].targetObjectID = first->objectID;
    authored->steps[0].quantize.timing = QuantizeTiming::Immediate;
    authored->steps[0].quantize.referenceObjectID = UINT32_MAX;
    authored->steps[0].quantize.referenceName = QStringLiteral("Missing");
    authored->steps[0].quantize.ifSilent = QuantizeIfSilent::Drop;

    authored->steps[1].type = VActionStep::Type::Play;
    authored->steps[1].targetObjectID = first->objectID;
    authored->steps[1].quantize.timing = QuantizeTiming::NextMarker;
    authored->steps[1].quantize.referenceObjectID = second->objectID;
    authored->steps[1].quantize.referenceName = QStringLiteral("StaleName");

    authored->steps[2].type = VActionStep::Type::Stop;
    authored->steps[2].targetObjectID = first->objectID;
    authored->steps[2].quantize.timing = QuantizeTiming::ExitMarker;

    authored->steps[3].type = VActionStep::Type::Play;
    authored->steps[3].targetObjectID = first->objectID;
    authored->steps[3].quantize.timing = QuantizeTiming::NextMarker;
    authored->steps[3].quantize.referenceName = second->name;

    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->actions.size(), 1u);
    ASSERT_EQ(request->actions[0].steps.size(), 4u);
    EXPECT_EQ(request->actions[0].steps[0].quantizeReference.stableId, harkentest::StudioObjectId(0u));
    EXPECT_EQ(request->actions[0].steps[0].ifSilent, harkenstudiobuild::ActionIfSilentV1::Immediate);
    EXPECT_EQ(request->actions[0].steps[1].quantizeReference.stableId,
              second->runtimeId());
    EXPECT_EQ(request->actions[0].steps[2].quantizeReference.stableId, harkentest::StudioObjectId(0u));
    EXPECT_EQ(request->actions[0].steps[3].quantizeReference.stableId,
              second->runtimeId());
    EXPECT_EQ(request->actions[0].steps[1].quantizeTiming, harkenstudiobuild::ActionQuantizeTimingV1::NextMarker);
    EXPECT_EQ(request->actions[0].steps[2].quantizeTiming, harkenstudiobuild::ActionQuantizeTimingV1::ExitMarker);
    for (const auto& step : request->actions[0].steps) {
        EXPECT_FLOAT_EQ(step.cooldownSeconds, 0.0f);
        EXPECT_EQ(step.cooldownScope, harkenstudiobuild::ActionCooldownScopeV1::Global);
    }
    expectRequestCompiles(*request);

    authored->steps[1].quantize.referenceObjectID = UINT32_MAX;
    authored->steps[1].quantize.referenceName = second->name;
    error.clear();
    request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_TRUE(error.contains(QStringLiteral("QuantizedAction"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("step 1"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("quantize reference"))) << error.toStdString();

    authored->steps[1].quantize.referenceObjectID = variable->objectID;
    error.clear();
    request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    EXPECT_EQ(request, nullptr);
    EXPECT_TRUE(error.contains(QStringLiteral("QuantizedAction"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("step 1"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("quantize reference"))) << error.toStdString();
}

TEST(CurrentWorldProjectAdapter, AdmitsReferencedStopExitMarkerAndKeepsOtherMarkerGaps)
{
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(nullptr, &sound);
    auto action = makeItem(QStringLiteral("MarkerAction"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    appendChild(project.layers.front().rootItem, action);
    VActionStep step;
    step.targetObjectID = sound->objectID;
    step.quantize.referenceObjectID = sound->objectID;
    for (const auto type : {VActionStep::Type::Play, VActionStep::Type::Stop}) {
        for (const auto timing : {QuantizeTiming::EntryMarker, QuantizeTiming::ExitMarker}) {
            step.type = type;
            step.quantize.timing = timing;
            action->comp<VAction>()->steps = {step};
            QString error;
            CurrentWorldBuildStatus status = CurrentWorldBuildStatus::Ready;
            const bool supported = type == VActionStep::Type::Stop
                && timing == QuantizeTiming::ExitMarker;
            const auto request = buildCurrentWorldSyncRequest(
                project, std::nullopt, &error, &status);
            EXPECT_EQ(request != nullptr, supported) << error.toStdString();
            EXPECT_EQ(status, supported ? CurrentWorldBuildStatus::Ready
                                        : CurrentWorldBuildStatus::CapabilityUnavailable);
            if (supported) {
                ASSERT_NE(request, nullptr);
                ASSERT_EQ(request->actions.size(), 1u);
                ASSERT_EQ(request->actions.front().steps.size(), 1u);
                EXPECT_EQ(request->actions.front().steps.front().quantizeTiming,
                          harkenstudiobuild::ActionQuantizeTimingV1::ExitMarker);
                EXPECT_EQ(request->actions.front().steps.front()
                              .quantizeReference.stableId,
                          sound->runtimeId());
                expectRequestCompiles(*request);
            } else {
                EXPECT_TRUE(error.contains(QStringLiteral("MarkerAction")))
                    << error.toStdString();
                EXPECT_TRUE(error.contains(QStringLiteral("marker scheduling contract")))
                    << error.toStdString();
            }
            EXPECT_EQ(action->comp<VAction>()->steps[0].quantize.timing, timing);
        }
    }
    step.type = VActionStep::Type::Play;
    step.quantize = {};
    step.quantize.timing = QuantizeTiming::NextMarker;
    action->comp<VAction>()->steps = {step};
    QString error;
    CurrentWorldBuildStatus status = CurrentWorldBuildStatus::Ready;
    EXPECT_EQ(buildCurrentWorldSyncRequest(project, std::nullopt, &error, &status), nullptr);
    EXPECT_EQ(status, CurrentWorldBuildStatus::InvalidAuthoring);
    EXPECT_TRUE(error.contains(QStringLiteral("reference is unresolved")));
}

TEST(RuntimeCapabilityMatrixTest, DelayedImmediateStopOnTrackAndPieceIsReadyEverywhere)
{
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(nullptr, &sound);
    auto piece = makeItem(QStringLiteral("Piece"), ProjectItem::Type::Piece, 6, harkentest::StudioObjectId(6));
    auto track = makeItem(QStringLiteral("Track"), ProjectItem::Type::Track, 7, harkentest::StudioObjectId(7));
    auto action = makeItem(QStringLiteral("DelayedStop"), ProjectItem::Type::Action, 8, harkentest::StudioObjectId(8));
    auto trackSound = makeItem(QStringLiteral("TrackSound"), ProjectItem::Type::Sound, 9, harkentest::StudioObjectId(10));
    HarkenTest::setSharedSoundSource(*trackSound, sound->sourceID);
    appendChild(project.layers.front().rootItem, piece);
    appendChild(piece, track);
    appendChild(track, trackSound);
    appendChild(project.layers.front().rootItem, action);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = trackSound->guid;
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(9));
    auto& clips = track->comp<VTrack>()->clips;
    ASSERT_TRUE(clips.mintOccurrenceOrdinal(&clip));
    clips.items.push_back(clip);
    project.nextObjectID = 10;
    for (const auto& target : {track, piece}) {
        VActionStep play;
        play.targetObjectID = target->objectID;
        VActionStep stop;
        stop.type = VActionStep::Type::Stop;
        stop.targetObjectID = target->objectID;
        stop.delay = 50.0f;
        stop.fadeTime = 40.0f;
        action->comp<VAction>()->steps = {play, stop};
        QString error;
        CurrentWorldBuildStatus status = CurrentWorldBuildStatus::Ready;
        const auto request = buildCurrentWorldSyncRequest(
            project, std::nullopt, &error, &status);
        ASSERT_NE(request, nullptr) << error.toStdString();
        EXPECT_EQ(status, CurrentWorldBuildStatus::Ready);
        ASSERT_EQ(request->actions.size(), 1u);
        ASSERT_EQ(request->actions.front().steps.size(), 2u);
        EXPECT_FLOAT_EQ(request->actions.front().steps[1].delayMilliseconds,
                        50.0f);
        EXPECT_FLOAT_EQ(request->actions.front().steps[1].fadeMilliseconds,
                        40.0f);
        expectRequestCompiles(*request);
        for (const O operation : {O::PackBuild, O::LiveSync, O::ActionRegistration,
                                  O::ProjectInputCompile, O::CompiledPreview, O::AuditionPlay}) {
            const auto assessment = RuntimeCapabilityMatrix::assess(*action, project, operation);
            EXPECT_EQ(assessment.state, S::Ready);
            EXPECT_TRUE(assessment.permitted());
        }
        EXPECT_FLOAT_EQ(action->comp<VAction>()->steps[1].delay, 50.0f);
        EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*action, project, O::Delete).permitted());
        action->comp<VAction>()->steps[1].delay = 0.0f;
        EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*action, project, O::LiveSync).permitted());
        play.delay = 50.0f;
        action->comp<VAction>()->steps = {play};
        EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*action, project, O::LiveSync).permitted());
    }
    VActionStep stop;
    stop.type = VActionStep::Type::Stop;
    stop.targetObjectID = sound->objectID;
    stop.delay = 50.0f;
    stop.fadeTime = 40.0f;
    action->comp<VAction>()->steps = {stop};
    EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*action, project, O::LiveSync).permitted());
}

TEST(RuntimeCapabilityMatrixTest, ActionOperationsMarkerAssessmentMatchesCurrentWorld)
{
    using T = VActionStep::Type;
    using Q = QuantizeTiming;
    using S = RuntimeCapabilityState;
    using O = RuntimeCapabilityOperation;
    using F = RuntimeCapabilityFeature;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(nullptr, &sound);
    auto action = makeItem(QStringLiteral("MarkerCapability"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    auto posted = makeItem(QStringLiteral("PostedAction"), ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    appendChild(project.layers.front().rootItem, action);
    appendChild(project.layers.front().rootItem, posted);
    VActionStep postedPlay;
    postedPlay.targetObjectID = sound->objectID;
    posted->comp<VAction>()->steps = {postedPlay};
    const struct {
        T type;
        Q timing;
        uint32_t reference;
        QString referenceName;
        S expected;
    } cases[] = {
        {T::Play, Q::Immediate, UINT32_MAX, {}, S::Ready},
        {T::Stop, Q::Immediate, UINT32_MAX, {}, S::Ready},
        {T::PostAction, Q::Immediate, UINT32_MAX, {}, S::Ready},
        {T::Play, Q::NextBeat, sound->objectID, {}, S::Ready},
        {T::Play, Q::NextBar, sound->objectID, {}, S::Ready},
        {T::Play, Q::NextMarker, sound->objectID, {}, S::Ready},
        {T::PostAction, Q::NextBeat, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::NextBar, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::NextMarker, sound->objectID, {}, S::UnavailableEngineContract},
        {T::Stop, Q::NextBeat, sound->objectID, {}, S::Ready},
        {T::Stop, Q::NextBar, sound->objectID, {}, S::Ready},
        {T::Stop, Q::NextMarker, sound->objectID, {}, S::Ready},
        {T::Play, Q::NextMarker, 0, sound->name, S::Ready},
        {T::Stop, Q::NextMarker, 0, {}, S::Ready},
        {T::Stop, Q::NaturalEnd, 0, {}, S::Ready},
        {T::Stop, Q::CurrentLoopEnd, 0, {}, S::Ready},
        {T::Play, Q::NextMarker, 0, {}, S::InvalidAuthoring},
        {T::Play, Q::NextMarker, UINT32_MAX, sound->name, S::InvalidAuthoring},
        {T::Stop, Q::NextMarker, UINT32_MAX, {}, S::InvalidAuthoring},
        {T::Stop, Q::ExitMarker, 0, {}, S::Ready},
        {T::Stop, Q::ExitMarker, sound->objectID, {}, S::Ready},
        {T::Stop, Q::ExitMarker, 0, sound->name, S::Ready},
        {T::Play, Q::ExitMarker, sound->objectID, {}, S::UnavailableEngineContract},
        {T::Play, Q::NaturalEnd, sound->objectID, {}, S::UnavailableEngineContract},
        {T::Play, Q::CurrentLoopEnd, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::ExitMarker, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::NaturalEnd, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::CurrentLoopEnd, sound->objectID, {}, S::UnavailableEngineContract},
        {T::Play, Q::EntryMarker, sound->objectID, {}, S::UnavailableEngineContract},
        {T::Stop, Q::EntryMarker, sound->objectID, {}, S::UnavailableEngineContract},
        {T::PostAction, Q::EntryMarker, sound->objectID, {}, S::UnavailableEngineContract},
    };
    for (const auto& test : cases) {
        for (const auto absent : {QuantizeIfSilent::Immediate, QuantizeIfSilent::Drop}) {
            SCOPED_TRACE(::testing::Message()
                << "type=" << static_cast<int>(test.type)
                << " timing=" << static_cast<int>(test.timing)
                << " reference=" << test.reference
                << " name=" << test.referenceName.toStdString()
                << " absent=" << static_cast<int>(absent));
            VActionStep step;
            step.type = test.type;
            step.targetObjectID = test.type == T::PostAction
                ? posted->objectID : sound->objectID;
            step.delay = 17.0f;
            step.quantize.timing = test.timing;
            step.quantize.referenceObjectID = test.reference;
            step.quantize.referenceName = test.referenceName;
            step.quantize.ifSilent = absent;
            action->comp<VAction>()->steps = {step};

            QString error;
            CurrentWorldBuildStatus status = CurrentWorldBuildStatus::Ready;
            const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error, &status);
            EXPECT_EQ(request != nullptr, test.expected == S::Ready) << error.toStdString();
            if (request)
                expectRequestCompiles(*request);
            const auto assessment = RuntimeCapabilityMatrix::assess(*action, project, O::LiveSync);
            EXPECT_EQ(assessment.state, test.expected);
            EXPECT_EQ(assessment.permitted(), test.expected == S::Ready);
            if (test.expected != S::Ready) {
                ASSERT_EQ(assessment.issues.size(), 1u);
                EXPECT_EQ(assessment.issues[0].diagnostic, error);
            }
            for (const F feature : {F::ActionTarget, F::Quantize, F::IfSilent,
                                    test.type == T::Play ? F::ActionPlay
                                      : (test.type == T::Stop ? F::ActionStop
                                                             : F::ActionPost)}) {
                const auto entry = RuntimeCapabilityMatrix::evaluate(
                    *action, project, O::LiveSync, feature);
                EXPECT_EQ(entry.state, test.expected);
                EXPECT_EQ(entry.diagnostic, error);
            }
            for (const O operation : {O::PackBuild, O::ActionRegistration,
                                      O::ProjectInputCompile, O::CompiledPreview, O::AuditionPlay}) {
                const auto current = RuntimeCapabilityMatrix::assess(*action, project, operation);
                EXPECT_EQ(current.state, test.expected);
                EXPECT_EQ(current.permitted(), test.expected == S::Ready);
                EXPECT_EQ(current.backend, operation == O::PackBuild
                    ? RuntimeCapabilityBackend::Pack : RuntimeCapabilityBackend::CurrentWorld);
            }
        }
    }
    for (const F feature : {F::Quantize, F::IfSilent}) {
        const auto live = RuntimeCapabilityMatrix::query(ProjectItem::Type::Action, O::LiveSync, feature);
        ASSERT_TRUE(live);
        EXPECT_EQ(live->state, S::Ready);
        for (const O operation : {O::PackBuild, O::ActionRegistration}) {
            const auto current = RuntimeCapabilityMatrix::query(ProjectItem::Type::Action, operation, feature);
            ASSERT_TRUE(current);
            EXPECT_EQ(current->state, S::Ready);
        }
    }
}

TEST(RuntimeCapabilityMatrixTest, ActionLiveSyncIgnoresUnrelatedActionsAndValidatesPostClosure)
{
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    using F = RuntimeCapabilityFeature;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(nullptr, &sound);
    const auto layer = project.layers.front().rootItem;
    auto root = makeItem(QStringLiteral("Root"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    auto posted = makeItem(QStringLiteral("Posted"), ProjectItem::Type::Action, 7, harkentest::StudioObjectId(7));
    auto other = makeItem(QStringLiteral("Other"), ProjectItem::Type::Action, 8, harkentest::StudioObjectId(8));
    project.nextObjectID = 9;
    for (const auto& action : {root, posted, other})
        appendChild(layer, action);
    VActionStep play;
    play.targetObjectID = sound->objectID;
    root->comp<VAction>()->steps = {play};
    VActionStep post;
    post.type = VActionStep::Type::PostAction;
    post.targetName = other->name;
    posted->comp<VAction>()->steps = {post};

    for (const S expected : {S::InvalidAuthoring, S::UnavailableEngineContract}) {
        SCOPED_TRACE(static_cast<int>(expected));
        VActionStep invalid = play;
        if (expected == S::InvalidAuthoring)
            invalid.targetObjectID = UINT32_MAX;
        else
            invalid.quantize.timing = QuantizeTiming::EntryMarker;
        other->comp<VAction>()->steps = {invalid};
        root->comp<VAction>()->steps = {play};
        EXPECT_EQ(buildCurrentWorldSyncRequest(project, std::nullopt), nullptr);
        EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::Ready);
        EXPECT_EQ(RuntimeCapabilityMatrix::assess(*other, project, O::LiveSync).state, expected);
        for (const F feature : {F::ActionTarget, F::ActionPlay, F::Quantize, F::IfSilent})
            EXPECT_EQ(RuntimeCapabilityMatrix::evaluate(*root, project, O::LiveSync, feature).state,
                      S::Ready);

        VActionStep invoke;
        invoke.type = VActionStep::Type::PostAction;
        invoke.targetObjectID = posted->objectID;
        root->comp<VAction>()->steps = {invoke};
        const auto assessment = RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync);
        EXPECT_EQ(assessment.state, expected);
        EXPECT_FALSE(assessment.permitted());
    }
}

TEST(RuntimeCapabilityMatrixTest, ActionLiveSyncScopesContentTargetsAndMarkerReferences)
{
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, nullptr);
    sound = container->children.back();
    const auto layer = project.layers.front().rootItem;
    auto root = makeItem(QStringLiteral("Root"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    auto reference = makeItem(QStringLiteral("Reference"), ProjectItem::Type::Container, 7, harkentest::StudioObjectId(7));
    auto child = makeItem(QStringLiteral("ReferenceSound"), ProjectItem::Type::Sound, 8, harkentest::StudioObjectId(8));
    project.nextObjectID = 9;
    appendChild(layer, root);
    appendChild(layer, reference);
    appendChild(reference, child);
    child->sourceID = 103;
    HarkenTest::setSharedSoundSource(*child, 103);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*reference, child->guid));
    reference->comp<VNX::VSelection>()->behavior.avoidRepeat = -1;
    VActionStep step;
    step.targetObjectID = sound->objectID;
    step.quantize.referenceObjectID = reference->objectID;
    root->comp<VAction>()->steps = {step};
    EXPECT_EQ(buildCurrentWorldSyncRequest(project, std::nullopt), nullptr);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::Ready);

    for (const VActionStep::Type type : {VActionStep::Type::Play, VActionStep::Type::Stop,
                                        VActionStep::Type::Reset}) {
        step.type = type;
        step.targetObjectID = reference->objectID;
        root->comp<VAction>()->steps = {step};
        EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state,
                  S::InvalidAuthoring);
    }
    step.type = VActionStep::Type::Play;
    step.targetObjectID = sound->objectID;
    step.quantize.timing = QuantizeTiming::NextMarker;
    for (const bool byName : {false, true}) {
        SCOPED_TRACE(byName);
        step.quantize.referenceObjectID = byName ? 0 : reference->objectID;
        step.quantize.referenceName = byName ? reference->name : QString();
        root->comp<VAction>()->steps = {step};
        EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state,
                  S::InvalidAuthoring);
    }
    auto track = makeItem(QStringLiteral("Track"), ProjectItem::Type::Track, 9, harkentest::StudioObjectId(9));
    appendChild(layer, track);
    std::erase(layer->children, reference);
    appendChild(track, reference);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    clip.targetGuid = reference->guid;
    clip.duration = 1000000;
    auto* timeline = track->comp<VTrack>();
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    project.nextObjectID = 10;
    VActionStep playTrack;
    playTrack.targetObjectID = track->objectID;
    root->comp<VAction>()->steps = {playTrack};
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::InvalidAuthoring);

    root->comp<VAction>()->steps = {step};
    reference->comp<VNX::VSelection>()->behavior.avoidRepeat = 0;
    CurrentWorldBuildOptions options;
    options.actionRoot = root.get();
    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->actions.size(), 1u);
    ASSERT_EQ(request->actions.front().steps.size(), 1u);
    EXPECT_EQ(request->actions.front().steps.front().quantizeReference.stableId,
              reference->runtimeId());
    EXPECT_TRUE(std::any_of(request->materializations.begin(), request->materializations.end(),
                           [&](const auto& row) { return row.stableId == child->runtimeId(); }));
    EXPECT_EQ(request->soundSources.size(), 2u);
    expectRequestCompiles(*request);

    root->comp<VAction>()->steps = {playTrack};
    request = buildCurrentWorldSyncRequest(project, std::nullopt, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->soundSources.size(), 1u);
    EXPECT_EQ(request->soundSources.front().sourceId, 103u);
    expectRequestCompiles(*request);
}

TEST(RuntimeCapabilityMatrixTest, ActionLiveSyncScopesVariablesAndKeepsGridDependencies)
{
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    const auto layer = project.layers.front().rootItem;
    auto root = makeItem(QStringLiteral("Root"), ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    auto variable = makeItem(QStringLiteral("State"), ProjectItem::Type::Variable, 7, harkentest::StudioObjectId(7));
    project.nextObjectID = 8;
    appendChild(layer, root);
    appendChild(layer, variable);
    variable->components.remove<VVariable>();
    VActionStep step;
    step.targetObjectID = sound->objectID;
    root->comp<VAction>()->steps = {step};
    EXPECT_EQ(buildCurrentWorldSyncRequest(project, std::nullopt), nullptr);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::Ready);

    step.type = VActionStep::Type::SetVariable;
    step.targetObjectID = variable->objectID;
    root->comp<VAction>()->steps = {step};
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::InvalidAuthoring);
    step.type = VActionStep::Type::Play;
    step.targetObjectID = container->objectID;
    root->comp<VAction>()->steps = {step};
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(sound->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*root, project, O::LiveSync).state, S::InvalidAuthoring);

    variable->components.getOrCreate<VVariable>()->kind = VariableKind::Step;
    CurrentWorldBuildOptions options;
    options.actionRoot = root.get();
    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->variables.size(), 1u);
    EXPECT_EQ(request->variables.front().variableId, variable->runtimeId());
    expectRequestCompiles(*request);
}

TEST(RuntimeCapabilityMatrixTest, ScopedSoundKeepsAncestorPropertiesWithoutExecutingItsSelection)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(&container, &sound);
    const auto other = container->children.back();
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.find(0)->weight, 0);
    CurrentWorldBuildOptions options;
    options.definitionRoot = sound.get();
    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        project, std::nullopt, &error, nullptr, &options);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers.front().mode, harkenstudiobuild::ContainerModeV1::Sequence);
    EXPECT_EQ(request->containers.front().containerStableId, container->runtimeId());
    EXPECT_EQ(request->containers.front().children.size(), 1u);
    EXPECT_EQ(std::count_if(request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) { return node.guid == other->runtimeId(); }), 0);
    expectRequestCompiles(*request);
    EXPECT_EQ(container->comp<VNX::VSelection>()->mode, SelectionMode::Random);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.find(0)->weight, 0);
    EXPECT_TRUE(RuntimeCapabilityMatrix::assess(
        *sound, project, RuntimeCapabilityOperation::AuditionPlay).permitted());
    container->comp<VNX::VSelection>()->selections.shared.find(1)->weight = 0;
    EXPECT_FALSE(RuntimeCapabilityMatrix::assess(
        *container, project, RuntimeCapabilityOperation::AuditionPlay).permitted());
    EXPECT_TRUE(RuntimeCapabilityMatrix::assess(
        *sound, project, RuntimeCapabilityOperation::AuditionPlay).permitted());
}

TEST(RuntimeCapabilityMatrixTest, AbsoluteSchedulesUseCurrentWorldAcrossOperations)
{
    using T = ProjectItem::Type;
    using F = RuntimeCapabilityFeature;
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(nullptr, &sound);
    const auto layer = project.layers.front().rootItem;
    auto piece = makeItem(QStringLiteral("Absolute"), T::Piece, 6, harkentest::StudioObjectId(6));
    auto track = makeItem(QStringLiteral("Staggered"), T::Track, 7, harkentest::StudioObjectId(7));
    auto nested = makeItem(QStringLiteral("Nested"), T::Piece, 8, harkentest::StudioObjectId(8));
    auto nestedTrack = makeItem(QStringLiteral("NestedTrack"), T::Track, 9, harkentest::StudioObjectId(9));
    auto trackSound = makeItem(QStringLiteral("TrackSound"), T::Sound, 10, harkentest::StudioObjectId(14));
    auto nestedSound = makeItem(QStringLiteral("NestedSound"), T::Sound, 11, harkentest::StudioObjectId(15));
    for (const auto& ownedSound : {trackSound, nestedSound}) {
        HarkenTest::setSharedSoundSource(*ownedSound, sound->sourceID);
    }
    appendChild(layer, piece);
    appendChild(piece, track);
    appendChild(track, trackSound);
    appendChild(track, nested);
    appendChild(nested, nestedTrack);
    appendChild(nestedTrack, nestedSound);
    auto appendClip = [&](ProjectItem& owner, const ProjectItem& target, int64_t start, uint64_t id) {
        Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(id));
        clip.targetGuid = target.guid;
        clip.start = start;
        clip.duration = 20000000;
        auto& clips = owner.comp<VTrack>()->clips;
        EXPECT_TRUE(clips.mintOccurrenceOrdinal(&clip));
        clips.items.push_back(clip);
    };
    appendClip(*track, *trackSound, 0, 10);
    appendClip(*track, *trackSound, 80000000, 11);
    appendClip(*track, *nested, 240000000, 12);
    appendClip(*nestedTrack, *nestedSound, 0, 13);
    project.nextObjectID = 12;
    for (const auto& object : {track, piece, nested}) {
        const F feature = object->type == T::Track ? F::TrackProgram : F::PieceProgram;
        for (const O operation : {O::PackBuild, O::LiveSync, O::ProjectInputCompile,
                                  O::CompiledPreview, O::AuditionPlay, O::Delete}) {
            SCOPED_TRACE(static_cast<int>(operation));
            const auto entry = RuntimeCapabilityMatrix::query(object->type, operation, feature);
            ASSERT_TRUE(entry);
            EXPECT_EQ(entry->state, S::Ready);
            const auto assessment = RuntimeCapabilityMatrix::assess(*object, project, operation);
            EXPECT_EQ(assessment.state, S::Ready) << QJsonDocument(assessment.toJson()).toJson().constData();
            EXPECT_TRUE(assessment.permitted());
            if (operation != O::Delete)
                EXPECT_TRUE(QJsonDocument(assessment.toJson()).toJson().contains("scheduling window"));
            EXPECT_EQ(assessment.backend, operation == O::PackBuild
                ? RuntimeCapabilityBackend::Pack : RuntimeCapabilityBackend::CurrentWorld);
        }
    }
    EXPECT_EQ(RuntimeCapabilityMatrix::evaluate(*track, project, O::AuditionPlay,
                  F::ClipAuthoredDuration).state, S::Ready);
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    track->comp<VTrack>()->clips.items[1].start = 80;
    track->comp<VTrack>()->clips.items[2].start = 240;
    EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*piece, project, O::AuditionPlay).permitted());
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec{};
    track->comp<VTrack>()->clips.items[1].start = 80000000;
    track->comp<VTrack>()->clips.items[2].start = 240000000;
    const auto before = RuntimeCapabilityMatrix::assess(*piece, project, O::AuditionPlay);
    track->comp<VTrack>()->clips.items.back().targetGuid = QUuid::createUuid();
    const auto invalid = RuntimeCapabilityMatrix::assess(*piece, project, O::AuditionPlay);
    EXPECT_EQ(invalid.state, S::InvalidAuthoring);
    EXPECT_FALSE(invalid.permitted());
    EXPECT_TRUE(RuntimeCapabilityMatrix::assess(*piece, project, O::Delete).permitted());
    EXPECT_TRUE(before.permitted());
}

TEST(RuntimeCapabilityMatrixTest, PreservesBindingEffectAndPlatformLimits)
{
    using F = RuntimeCapabilityFeature;
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    std::shared_ptr<ProjectItem> sound;
    auto project = makeRandomProject(nullptr, &sound);
    auto variable = makeItem(QStringLiteral("Gain"), ProjectItem::Type::Variable, 6, harkentest::StudioObjectId(6));
    appendChild(project.layers.front().rootItem, variable);
    auto binding = BindingSemantics::defaultBinding(BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(7));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));
    EXPECT_EQ(RuntimeCapabilityMatrix::evaluate(*sound, project, O::PackBuild, F::TypedBinding).state,
              S::Ready);
    binding.variable = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*sound, project, O::PackBuild).state, S::InvalidAuthoring);
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.remove(binding.runtimeId()));
    EffectEntry effect;
    effect.guid = VNX::authoringUuid(harkentest::StudioObjectId(8));
    effect.type = EffectType::Gain;
    effect.parameters = EffectChainSemantics::defaultParameters(effect.type);
    ASSERT_TRUE(sound->comp<VMixNode>()->effects.replace({effect}));
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*sound, project, O::PackBuild).state, S::Ready);
    effect.bypass = true;
    ASSERT_TRUE(sound->comp<VMixNode>()->effects.replace({effect}));
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*sound, project, O::PackBuild).state,
              S::UnavailableEngineContract);
    ASSERT_TRUE(sound->comp<VMixNode>()->effects.replace({}));
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*sound, project, O::PackBuild,
                  BuildVariant{QUuid::createUuid().toString(QUuid::WithoutBraces), {}}).state,
              S::InvalidAuthoring);
    project.channelConfig = AudioChannelConfig::Atmos_7_1_4;
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*sound, project, O::AuditionPlay).state,
              S::UnavailableEngineContract);
}

TEST(RuntimeCapabilityMatrixTest, ResourceCapabilitiesRespectOperationAndAuthoring)
{
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    auto project = makeRandomProject(nullptr, nullptr);
    const auto bus = project.busses.front().rootItem;
    auto attenuation = makeItem(QStringLiteral("Attenuation"), ProjectItem::Type::Attenuation, 6, harkentest::StudioObjectId(6));
    project.attenuationItems.push_back({attenuation, {}});
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*bus, project, O::AuditionPlay).state,
              S::UnavailableEngineContract);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::AuditionPlay).state,
              S::UnavailableEngineContract);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::ProjectInputCompile).state,
              S::UnavailableEngineContract);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::PackBuild).state, S::Ready);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::LiveSync).backend,
              RuntimeCapabilityBackend::Native);
    auto& def = *attenuation->comp<VAttenuation>();
    def.maxDistance = 0.0f;
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::PackBuild).state,
              S::InvalidAuthoring);
    def.maxDistance = 100.0f;
    auto& pitch = def.dimensions.emplaceBack(AttenuationDimensionDef{AttenuationDimension::Pitch});
    pitch.curve = attenuationDefaultCurve(AttenuationDimension::Pitch, 100.0f);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::LiveSync).state,
              S::UnavailableEngineContract);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(*attenuation, project, O::Delete).state,
              S::UnavailableEngineContract);
}

TEST(CurrentWorldArtifactWriter, WritesCanonicalStudioAuthoredActionWorld)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    auto action = makeItem(QStringLiteral("ArtifactAction"),
                           ProjectItem::Type::Action, 6, harkentest::StudioObjectId(6));
    appendChild(layer, action);
    VActionStep play;
    play.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(play);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    for (auto& row : request->materializations) {
        if (row.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
            row.contentRevision = std::hash<harkenstudio::ObjectId>{}(row.stableId) + 1000;
    }
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request->project);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request->variables, request->materializations,
        request->containers, {},
        request->transitions, request->actions);
    ASSERT_TRUE(compiled.ok());
    ASSERT_NE(compiled.world, nullptr);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("action.vnworld"));
    ASSERT_TRUE(WriteCurrentWorldArtifactV1(path, compiled.world->View(),
                                            &error))
        << error.toStdString();

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray actual = file.readAll();
    std::vector<std::uint8_t> expected;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(
        compiled.world->View(), expected));
    ASSERT_EQ(actual.size(), static_cast<qsizetype>(expected.size()));
    EXPECT_EQ(std::memcmp(actual.constData(), expected.data(), expected.size()),
              0);
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
        artifactDirectWriteFallback = true;
        QString error;
        EXPECT_FALSE(WriteCurrentWorldArtifactV1(
            path, compiled.world->View(), &error));
        EXPECT_FALSE(artifactDirectWriteFallback);
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
    EXPECT_EQ(world.actions[0].steps[0].opcode,
              harken::ActionOpcodeV1::SetVariable);
    EXPECT_EQ(world.actions[0].steps[0].scope,
              harken::ActionScopeV1::Emitter);
}

TEST(CurrentWorldProjectAdapter, ClassifiesOnlyRuntimeRelevantMutations)
{
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Layer, ProjectItem::Type::Bus}) {
        EXPECT_TRUE(affectsCurrentWorld(type, CurrentWorldMutationKind::Property,
                                       QStringLiteral("bindings")));
        EXPECT_TRUE(affectsCurrentWorld(type, CurrentWorldMutationKind::Property,
                                       QStringLiteral("bindings.voice.volume")));
        EXPECT_FALSE(affectsCurrentWorld(type, CurrentWorldMutationKind::Property,
                                        QStringLiteral("bindingsUnrelated")));
    }
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Sound,
                                    CurrentWorldMutationKind::Structure));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Sound,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("distinguishLanguage")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Container,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("behavior")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Track,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("clips")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Piece,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("markers")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Variable,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("rangeMin")));

    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Action,
                                    CurrentWorldMutationKind::Structure));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Action,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("steps")));
    EXPECT_FALSE(affectsCurrentWorld(ProjectItem::Type::Variable,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("scope")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Bus,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("volume")));
    EXPECT_FALSE(affectsCurrentWorld(ProjectItem::Type::Container,
                                     CurrentWorldMutationKind::Property,
                                     QStringLiteral("notes")));
    EXPECT_FALSE(affectsCurrentWorld(ProjectItem::Type::Container,
                                     CurrentWorldMutationKind::Property,
                                     QStringLiteral("color")));
    EXPECT_FALSE(affectsCurrentWorld(ProjectItem::Type::Container,
                                     CurrentWorldMutationKind::Property,
                                     QStringLiteral("expanded")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Container,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("blends")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Container,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("matches")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Container,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("blends")));
    EXPECT_TRUE(affectsCurrentWorld(ProjectItem::Type::Container,
                                    CurrentWorldMutationKind::Property,
                                    QStringLiteral("transitions")));
    EXPECT_FALSE(affectsCurrentWorld(ProjectItem::Type::Sound,
                                     CurrentWorldMutationKind::Property,
                                     QStringLiteral("unknownProperty")));
}

TEST(CurrentWorldProjectAdapter, ResolvesPlatformGroupsBeforePruningAnOwnershipContainer)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    container->comp<VNX::VSelection>()->selections.shared.entries.clear();
    const auto master = project.busses.front().rootItem;
    container->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(master->guid), -4.0f}};
    container->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(master->guid), -12.0f}};
    SpatialRatingFieldGroup rating;
    rating.spatialPanning = -0.25f;
    rating.spatialIs3DEnabled = true;
    rating.spatialAttenuationDistanceScaling = 2.0f;
    container->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, rating};
    sound->comp<VMixNode>()->outputBusId = 999;
    sound->comp<VMixNode>()->outputBusVolume = 20.0f;
    sound->comp<VMixNode>()->sends.push_back({999, 1.0f});
    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, HarkenTest::PlatformIds::Android, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_TRUE(request->containers.empty());
    EXPECT_EQ(std::count_if(request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& node) { return node.guid == container->runtimeId(); }), 0);
    const auto node = std::find_if(request->project.nodes.begin(), request->project.nodes.end(),
        [&](const auto& value) { return value.guid == sound->runtimeId(); });
    ASSERT_NE(node, request->project.nodes.end());
    EXPECT_EQ(node->presence.bits & kFieldGroupPropertyMask, kFieldGroupPropertyMask);
    EXPECT_FLOAT_EQ(node->outputBusVolume, -12.0f);
    EXPECT_EQ(node->mainOutputBusRef, master->runtimeId());
    EXPECT_TRUE(node->sends.empty());
    EXPECT_TRUE(node->spatial.is3DEnabled);
    EXPECT_FLOAT_EQ(node->spatial.panning, -0.25f);
    EXPECT_FLOAT_EQ(node->spatial.spread, 0.0f);
    EXPECT_FLOAT_EQ(node->spatial.attenuationDistanceScaling, 2.0f);
    expectRequestCompiles(*request);
}

TEST(CurrentWorldProjectAdapter, EffectiveGroupsMoveChildFingerprintsAndHistoryDoesNot)
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> sound;
    ProjectData project = makeRandomProject(&container, &sound);
    const auto master = project.busses.front().rootItem;
    container->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(master->guid), -4.0f}};
    const auto compile = [&]() -> std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> {
        QString error;
        const auto request = buildCurrentWorldSyncRequest(project, {}, &error);
        EXPECT_NE(request, nullptr) << error.toStdString();
        if (!request) return {};
        for (auto& materialization : request->materializations)
            if (materialization.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
                materialization.contentRevision = std::hash<harkenstudio::ObjectId>{}(materialization.stableId) + 1000;
        const auto front = harkenprojectschema::CompileFrontEnd(request->project);
        EXPECT_TRUE(front.ok());
        if (!front.project) return {};
        auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
            *front.project, request->variables, request->materializations, request->containers,
            {}, request->transitions, request->actions);
        EXPECT_TRUE(compiled.ok());
        return std::move(compiled.world);
    };
    const auto fingerprint = [&](const harkencurrentworldcompiler::OwningCurrentWorldV1& world) {
        const auto& view = world.View();
        for (uint32_t i = 0; i < view.playablePropertyCount; ++i)
            if (view.playableProperties[i].stableId == sound->runtimeId())
                return view.playableProperties[i].programSemanticFingerprint;
        return uint64_t{0};
    };
    const auto first = compile();
    ASSERT_NE(first, nullptr);
    const auto before = fingerprint(*first);
    ASSERT_NE(before, 0u);
    sound->fieldGroups.mainOutput.shared.localHistory =
        MainOutputFieldGroup{toFieldGroupGuid(QUuid::createUuid()), -19.0f};
    const auto history = compile();
    ASSERT_NE(history, nullptr);
    EXPECT_EQ(fingerprint(*history), before);
    container->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -8.0f;
    const auto changed = compile();
    ASSERT_NE(changed, nullptr);
    const auto after = fingerprint(*changed);
    EXPECT_NE(after, before);
    bool found = false;
    for (uint32_t i = 0; i < changed->View().playableOccurrenceCount; ++i) {
        const auto& occurrence = changed->View().playableOccurrences[i];
        if (occurrence.childPlayable.stableId != sound->runtimeId()) continue;
        found = true;
        EXPECT_EQ(occurrence.childPlayable.expectedProgramSemanticFingerprint, after);
        EXPECT_EQ(occurrence.payload.presentMask & kFieldGroupPropertyMask, 0u);
    }
    EXPECT_TRUE(found);
}

} // namespace VNX
