#include "Widgets/Blueprint/SignalPathGainEditing.h"
#include "Widgets/Blueprint/SignalPathGraph.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using namespace VNX;
using T = ProjectItemType;

class SignalPathGainEditingTest : public ::testing::Test
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
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("SignalPathGainEditing");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        master = manager.getMasterBusItem();
        b1 = create("B1", T::Bus, master);
        b2 = create("B2", T::Bus, master);
        b3 = create("B3", T::Bus, master);
        layer = create("Layer", T::Layer);
        container = create("Container", T::Container, layer);
        s1 = create("S1", T::Sound, container);
        s2 = create("S2", T::Sound, container);
        layer->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -7.0f}};
        layer->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f},
                             {toFieldGroupGuid(b3->guid), -3.0f}}}};
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerFieldGroupActions(actions, &manager);
        UndoRedoManager::instance().clear();
        for (auto* item : {master, b1, b2, b3, layer, container, s1, s2}) item->clearDirty();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        if (!item) throw std::runtime_error("Cannot create test object");
        return item;
    }

    ProjectData& project()
    {
        return *ProjectManager::instance().currentProject();
    }

    SignalPathGraph graph(FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        auto result = buildSignalPathGraph(project(), {{s1->guid, {}}, {s2->guid, {}}}, scope);
        EXPECT_TRUE(result.ok());
        return result;
    }

    size_t edge(const SignalPathGraph& value, SignalPathEdgeKind kind,
                const ProjectItem* from, const ProjectItem* to, const ProjectItem* provider = nullptr)
    {
        for (size_t index = 0; index < value.edges.size(); ++index) {
            const auto& candidate = value.edges[index];
            if (candidate.kind == kind
                && value.nodes[candidate.from.occurrenceIndex].key.objectGuid == from->guid
                && value.nodes[candidate.to.occurrenceIndex].key.objectGuid == to->guid
                && (!provider || (candidate.routingSource && candidate.routingSource->objectGuid == provider->guid)))
                return index;
        }
        throw std::runtime_error("Expected connection is missing");
    }

    EffectiveDefinitionFieldGroups effective(
        const ProjectItem* item, FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(project(), scope);
        EXPECT_TRUE(snapshot.ok());
        return snapshot.definitions.at(toFieldGroupGuid(item->guid));
    }

    float sendDb(const ProjectItem* item, const ProjectItem* target,
                 FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        const auto value = effective(item, scope);
        for (const auto& row : value.send.sends)
            if (row.targetBusID == toFieldGroupGuid(target->guid)) return row.sendLevel;
        throw std::runtime_error("Expected Send is missing");
    }

    float gain(const SignalPathGraph& value, size_t index,
               const ProjectItem* previewItem = nullptr,
               const DefinitionFieldGroups* previewGroups = nullptr)
    {
        const auto result = signalPathGainValue(project(), value, index, previewItem, previewGroups);
        EXPECT_TRUE(result.has_value());
        return result.value();
    }

    QTemporaryDir directory;
    ProjectItem* master = nullptr;
    ProjectItem* b1 = nullptr;
    ProjectItem* b2 = nullptr;
    ProjectItem* b3 = nullptr;
    ProjectItem* layer = nullptr;
    ProjectItem* container = nullptr;
    ProjectItem* s1 = nullptr;
    ProjectItem* s2 = nullptr;
};

TEST_F(SignalPathGainEditingTest, InheritedOutputEditsProviderAndCommitsOneUndo)
{
    const auto before = layer->fieldGroups;
    const auto firstBefore = s1->fieldGroups;
    const auto secondBefore = s2->fieldGroups;
    const auto value = graph();
    const auto index = edge(value, SignalPathEdgeKind::Output, layer, b1, layer);
    EXPECT_FLOAT_EQ(gain(value, index), -7.0f);
    ProjectItem* previewItem = nullptr;
    std::optional<DefinitionFieldGroups> preview;
    QObject observer;
    QObject::connect(&ProjectManager::instance(), &ProjectManager::fieldGroupsPreviewed, &observer,
        [&](ProjectItem* item, const DefinitionFieldGroups& groups) {
            previewItem = item;
            preview = groups;
        });
    SignalPathGainEditSession session(value, index, &observer);
    ASSERT_TRUE(session.isActive());
    session.preview(-12.0);
    session.preview(-15.0);
    ASSERT_TRUE(preview);
    EXPECT_EQ(previewItem, layer);
    EXPECT_FLOAT_EQ(gain(value, index, previewItem, &*preview), -15.0f);
    EXPECT_EQ(layer->fieldGroups, before);
    EXPECT_FALSE(layer->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    session.commit();
    EXPECT_FALSE(session.isActive());
    for (const auto* source : {s1, s2}) {
        EXPECT_FLOAT_EQ(effective(source).mainOutput.outputBusVolume, -15.0f);
        EXPECT_EQ(effective(source).mainOutput.mainOutputBusRef, toFieldGroupGuid(b1->guid));
        EXPECT_EQ(source->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    }
    EXPECT_EQ(s1->fieldGroups, firstBefore);
    EXPECT_EQ(s2->fieldGroups, secondBefore);
    EXPECT_EQ(layer->fieldGroups.send, before.send);
    EXPECT_EQ(layer->fieldGroups.spatialRating, before.spatialRating);
    const auto after = layer->fieldGroups;
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, before);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(layer->fieldGroups, after);
}

TEST_F(SignalPathGainEditingTest, ContentSendPreviewCancelRestoresDisplayWithoutWriting)
{
    const auto before = layer->fieldGroups;
    const auto value = graph();
    const auto index = edge(value, SignalPathEdgeKind::Send, layer, b2, layer);
    EXPECT_FLOAT_EQ(gain(value, index), VMixNode::dbToLinear(-6.0f));
    std::optional<DefinitionFieldGroups> preview;
    QObject observer;
    QObject::connect(&ProjectManager::instance(), &ProjectManager::fieldGroupsPreviewed, &observer,
        [&](ProjectItem* item, const DefinitionFieldGroups& groups) {
            EXPECT_EQ(item, layer);
            preview = groups;
        });
    SignalPathGainEditSession session(value, index, &observer);
    ASSERT_TRUE(session.isActive());
    session.preview(0.75);
    ASSERT_TRUE(preview);
    EXPECT_FLOAT_EQ(gain(value, index, layer, &*preview), 0.75f);
    EXPECT_EQ(layer->fieldGroups, before);
    session.cancel();
    EXPECT_FALSE(session.isActive());
    ASSERT_TRUE(preview);
    EXPECT_EQ(*preview, before);
    EXPECT_EQ(layer->fieldGroups, before);
    EXPECT_FALSE(layer->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_FLOAT_EQ(sendDb(s1, b3), -3.0f);
}

TEST_F(SignalPathGainEditingTest, SelectedPlatformOutputPreservesSharedAndOtherPlatform)
{
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const auto before = layer->fieldGroups;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Output, layer, b1, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(-18.0);
    session.commit();
    EXPECT_EQ(layer->fieldGroups.mainOutput.shared, before.mainOutput.shared);
    EXPECT_FLOAT_EQ(effective(s1).mainOutput.outputBusVolume, -18.0f);
    EXPECT_FLOAT_EQ(effective(s2, HarkenTest::FieldGroupPlatformIds::Android).mainOutput.outputBusVolume, -7.0f);
    EXPECT_EQ(layer->fieldGroups.send, before.send);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, before);
}

TEST_F(SignalPathGainEditingTest, AllSendProvidersChangeTogetherAndPreserveOtherRows)
{
    container->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b2->guid), -11.0f},
                         {toFieldGroupGuid(b3->guid), -8.0f}}}};
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android},
        HarkenTest::PlatformIds::Windows, true);
    ASSERT_EQ(PlatformContextService::instance().context().selected.size(), 2);
    const auto layerBefore = layer->fieldGroups;
    const auto containerBefore = container->fieldGroups;
    const auto firstBefore = s1->fieldGroups;
    const auto secondBefore = s2->fieldGroups;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Send, layer, b2, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(VMixNode::dbToLinear(-12.0f));
    session.commit();
    EXPECT_NEAR(sendDb(s1, b2, std::nullopt), -12.0f, 0.0001f);
    EXPECT_FLOAT_EQ(sendDb(s1, b3, std::nullopt), -3.0f);
    EXPECT_EQ(container->fieldGroups.send.shared, containerBefore.send.shared);
    for (const auto* source : {s1, s2}) {
        EXPECT_NEAR(sendDb(source, b2), -12.0f, 0.0001f);
        EXPECT_NEAR(sendDb(source, b2, HarkenTest::FieldGroupPlatformIds::Android), -12.0f, 0.0001f);
        EXPECT_FLOAT_EQ(sendDb(source, b3), -3.0f);
        EXPECT_FLOAT_EQ(sendDb(source, b3, HarkenTest::FieldGroupPlatformIds::Android), -8.0f);
    }
    EXPECT_EQ(layer->fieldGroups.mainOutput, layerBefore.mainOutput);
    EXPECT_EQ(container->fieldGroups.mainOutput, containerBefore.mainOutput);
    EXPECT_EQ(s1->fieldGroups, firstBefore);
    EXPECT_EQ(s2->fieldGroups, secondBefore);
    const auto layerAfter = layer->fieldGroups;
    const auto containerAfter = container->fieldGroups;
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, layerBefore);
    EXPECT_EQ(container->fieldGroups, containerBefore);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(layer->fieldGroups, layerAfter);
    EXPECT_EQ(container->fieldGroups, containerAfter);
}

TEST_F(SignalPathGainEditingTest, AllOutputUnifiesGainWithoutChangingRoutingOrOtherGroups)
{
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}) {
        layer->fieldGroups.mainOutput.platforms[platform] = {FieldGroupMode::UseShared,
            MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -11.0f},
            FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)};
    }
    const auto before = layer->fieldGroups;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Output, layer, b1, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(-4.0);
    session.commit();
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -4.0f);
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}) {
        const auto resolved = effective(s1, platform);
        EXPECT_FLOAT_EQ(resolved.mainOutput.outputBusVolume, -4.0f);
        EXPECT_EQ(resolved.mainOutput.mainOutputBusRef, toFieldGroupGuid(b1->guid));
        const auto authored = layer->fieldGroups.mainOutput.platforms.find(platform);
        EXPECT_TRUE(authored == layer->fieldGroups.mainOutput.platforms.end()
            || (authored->second.localFields
                & FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)) == 0);
    }
    EXPECT_EQ(layer->fieldGroups.send, before.send);
    EXPECT_EQ(layer->fieldGroups.spatialRating, before.spatialRating);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, before);
}

TEST_F(SignalPathGainEditingTest, AllOutputPreservesPlatformsWithAnotherDestination)
{
    layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared,
        MainOutputFieldGroup{toFieldGroupGuid(master->guid), -11.0f}};
    const auto before = layer->fieldGroups;
    const auto androidBefore = effective(s1, HarkenTest::FieldGroupPlatformIds::Android).mainOutput;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Output, layer, b1, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(-4.0);
    session.commit();
    EXPECT_FLOAT_EQ(effective(s1, std::nullopt).mainOutput.outputBusVolume, -4.0f);
    EXPECT_FLOAT_EQ(effective(s1).mainOutput.outputBusVolume, -4.0f);
    EXPECT_EQ(effective(s1, HarkenTest::FieldGroupPlatformIds::Android).mainOutput, androidBefore);
    EXPECT_EQ(layer->fieldGroups.send, before.send);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, before);
}

TEST_F(SignalPathGainEditingTest, AllSendPreservesPlatformSpecificRowsAndDoesNotAddMissingTarget)
{
    layer->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::UseShared,
        SendFieldGroup{{{toFieldGroupGuid(b2->guid), -8.0f},
                         {toFieldGroupGuid(b3->guid), -4.0f}}}};
    layer->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared,
        SendFieldGroup{{{toFieldGroupGuid(b3->guid), -10.0f}}}};
    const auto before = layer->fieldGroups;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Send, layer, b2, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(VMixNode::dbToLinear(-15.0f));
    session.commit();
    EXPECT_NEAR(sendDb(s1, b2, std::nullopt), -15.0f, 0.0001f);
    EXPECT_NEAR(sendDb(s1, b2), -15.0f, 0.0001f);
    EXPECT_FLOAT_EQ(sendDb(s1, b3, std::nullopt), -3.0f);
    EXPECT_FLOAT_EQ(sendDb(s1, b3), -4.0f);
    const auto android = effective(s1, HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_EQ(android.send.sends.size(), 1u);
    EXPECT_EQ(android.send.sends.front().targetBusID, toFieldGroupGuid(b3->guid));
    EXPECT_FLOAT_EQ(android.send.sends.front().sendLevel, -10.0f);
    EXPECT_EQ(layer->fieldGroups.mainOutput, before.mainOutput);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, before);
}

TEST_F(SignalPathGainEditingTest, BusOutputPreviewCancelAndCommitPreservePlatformScope)
{
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*b1);
    ASSERT_TRUE(locator);
    const auto property = static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*b1, *locator, property, -4.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*b1, *locator, property, -8.0, HarkenTest::PlatformIds::Windows));
    const auto before = PlayablePropertyAuthoringV1::definitionToJson(*b1);
    const auto value = graph();
    const auto index = edge(value, SignalPathEdgeKind::Output, b1, master, b1);
    EXPECT_FLOAT_EQ(gain(value, index), -8.0f);
    SignalPathGainEditSession cancelled(value, index);
    ASSERT_TRUE(cancelled.isActive());
    cancelled.preview(-10.0);
    EXPECT_FLOAT_EQ(gain(value, index), -10.0f);
    EXPECT_FLOAT_EQ(b1->comp<VMixNode>()->outputBusVolume, -4.0f);
    cancelled.cancel();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    SignalPathGainEditSession session(value, index);
    ASSERT_TRUE(session.isActive());
    session.preview(-11.0);
    session.commit();
    EXPECT_FLOAT_EQ(gain(value, index), -11.0f);
    const auto after = PlayablePropertyAuthoringV1::definitionToJson(*b1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(b1->parent, master);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), after);
}

TEST_F(SignalPathGainEditingTest, BusSendPreviewCancelAndCommitPreserveOtherSends)
{
    auto* mix = b1->comp<VMixNode>();
    mix->sends = {{b2->objectID, 0.5f}, {b3->objectID, 0.25f}};
    b1->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Sends);
    const auto before = PlayablePropertyAuthoringV1::definitionToJson(*b1);
    const auto value = graph();
    const auto index = edge(value, SignalPathEdgeKind::Send, b1, b2, b1);
    EXPECT_FLOAT_EQ(gain(value, index), 0.5f);
    SignalPathGainEditSession cancelled(value, index);
    ASSERT_TRUE(cancelled.isActive());
    cancelled.preview(0.75);
    ASSERT_EQ(mix->sends.size(), 2u);
    EXPECT_FLOAT_EQ(mix->sends[0].sendLevel, 0.75f);
    EXPECT_FLOAT_EQ(mix->sends[1].sendLevel, 0.25f);
    cancelled.cancel();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    SignalPathGainEditSession session(value, index);
    ASSERT_TRUE(session.isActive());
    session.preview(0.75);
    session.commit();
    EXPECT_FLOAT_EQ(mix->sends[0].sendLevel, 0.75f);
    EXPECT_FLOAT_EQ(mix->sends[1].sendLevel, 0.25f);
    const auto after = PlayablePropertyAuthoringV1::definitionToJson(*b1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), after);
}

TEST_F(SignalPathGainEditingTest, ThroughAndStaleProviderCannotStartEditing)
{
    const auto value = graph();
    const auto through = edge(value, SignalPathEdgeKind::Through, s1, container);
    EXPECT_FALSE(signalPathGainValue(project(), value, through));
    SignalPathGainEditSession throughSession(value, through);
    EXPECT_FALSE(throughSession.isActive());
    SignalPathGainEditSession missingSession(value, value.edges.size());
    EXPECT_FALSE(missingSession.isActive());
    container->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -9.0f}};
    const auto before = layer->fieldGroups;
    const auto containerBefore = container->fieldGroups;
    SignalPathGainEditSession stale(value,
        edge(value, SignalPathEdgeKind::Output, layer, b1, layer));
    EXPECT_FALSE(stale.isActive());
    stale.preview(-12.0);
    stale.commit();
    EXPECT_EQ(layer->fieldGroups, before);
    EXPECT_EQ(container->fieldGroups, containerBefore);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathGainEditingTest, InspectorInheritedPreviewResolvesAllScopesAndRestoresMixedState)
{
    layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -11.0f},
        FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)};
    registerBuiltinInspectorCustomizations();
    PropertyEditorEngine inspector;
    inspector.setSchema(VMixNode::schema(), ComponentType::MixNode);
    inspector.setItemType(T::Sound);
    std::unique_ptr<QWidget> page(inspector.buildPage());
    inspector.bindItem(s1);
    auto* slider = page->findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_TRUE(slider->mixedValue());
    EXPECT_FALSE(slider->isEnabled());
    const auto original = s1->fieldGroups;
    const auto value = graph();
    SignalPathGainEditSession session(value,
        edge(value, SignalPathEdgeKind::Output, layer, b1, layer));
    ASSERT_TRUE(session.isActive());
    session.preview(-15.0);
    EXPECT_FALSE(slider->mixedValue());
    EXPECT_DOUBLE_EQ(slider->value(), -15.0);
    EXPECT_FALSE(slider->isEnabled());
    EXPECT_EQ(s1->fieldGroups, original);
    EXPECT_FLOAT_EQ(effective(s1).mainOutput.outputBusVolume, -7.0f);
    session.cancel();
    EXPECT_TRUE(slider->mixedValue());
    EXPECT_FALSE(slider->isEnabled());
    EXPECT_EQ(s1->fieldGroups, original);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathGainEditingTest, InspectorPreviewKeepsAuthoredEditGuard)
{
    registerBuiltinInspectorCustomizations();
    PropertyEditorEngine inspector;
    inspector.setSchema(VMixNode::schema(), ComponentType::MixNode);
    inspector.setItemType(T::Layer);
    std::unique_ptr<QWidget> page(inspector.buildPage());
    inspector.bindItem(layer);
    auto* slider = page->findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(slider, nullptr);
    auto preview = layer->fieldGroups;
    preview.mainOutput.shared.localHistory->outputBusVolume = -13.0f;
    emit ProjectManager::instance().fieldGroupsPreviewed(layer, preview);
    EXPECT_DOUBLE_EQ(slider->value(), -13.0);
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -7.0f);
    slider->simulateValueChange(-9.0);
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -9.0f);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_DOUBLE_EQ(slider->value(), -9.0);
}

TEST_F(SignalPathGainEditingTest, InspectorActiveParameterIgnoresExternalPreview)
{
    registerBuiltinInspectorCustomizations();
    PropertyEditorEngine inspector;
    inspector.setSchema(VMixNode::schema(), ComponentType::MixNode);
    inspector.setItemType(T::Layer);
    std::unique_ptr<QWidget> page(inspector.buildPage());
    inspector.bindItem(layer);
    auto* slider = page->findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-8.0);
    ASSERT_TRUE(slider->binding()->isEditing());
    auto preview = layer->fieldGroups;
    preview.mainOutput.shared.localHistory->outputBusVolume = -13.0f;
    emit ProjectManager::instance().fieldGroupsPreviewed(layer, preview);
    EXPECT_TRUE(slider->binding()->isEditing());
    EXPECT_DOUBLE_EQ(slider->value(), -8.0);
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -7.0f);
    slider->binding()->cancelEdit();
    EXPECT_FALSE(slider->binding()->isEditing());
    EXPECT_DOUBLE_EQ(slider->value(), -7.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

}
