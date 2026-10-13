#include "PropertyDescriptor.h"
#include "Core/Components/VMixNode.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "Widgets/Blueprint/SignalPathGraph.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QTemporaryDir>

#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

const VNX::PropertyDescriptor* findField(const VNX::PropertySchema& s, QLatin1String field)
{
    for (const auto& d : s)
        if (d.field == field) return &d;
    return nullptr;
}

} // namespace

TEST(RoutingInspectorSchema, SendsFieldPresentOnAllMixHostSchemas)
{
    for (const auto* schema : { &VNX::soundInspectorSchema(),
                                &VNX::layerInspectorSchema(),
                                &VNX::busInspectorSchema() })
    {
        const auto* sends = findField(*schema, QLatin1String("sends"));
        ASSERT_NE(sends, nullptr);
        EXPECT_EQ(sends->type, VNX::PropType::List);
        EXPECT_EQ(sends->section, VNX::PropertySection::Send);
        EXPECT_FALSE(sends->flags.readOnly);

        const auto* target = findField(sends->elementSchema, QLatin1String("targetBusID"));
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->type, VNX::PropType::ObjectRef);
        EXPECT_TRUE(static_cast<bool>(target->refSelectable));
    }
}

TEST(RoutingInspectorSchema, BusDerivedListsAreReadOnlyComputed)
{
    const auto& s = VNX::busInspectorSchema();
    for (const auto field : { QLatin1String("inputs"), QLatin1String("receives") })
    {
        const auto* d = findField(s, field);
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->type, VNX::PropType::List);
        EXPECT_TRUE(d->flags.readOnly);
        EXPECT_FALSE(d->flags.serialize);
        EXPECT_TRUE(static_cast<bool>(d->get));
        EXPECT_FALSE(static_cast<bool>(d->set));
        EXPECT_NE(findField(d->elementSchema, QLatin1String("source")), nullptr);
        EXPECT_NE(findField(d->elementSchema, QLatin1String("level")), nullptr);
    }
}

TEST(RoutingInspectorSchema, RouteCategoryOwnsRoutingSections)
{
    for (const auto section : {VNX::PropertySection::MainOutput, VNX::PropertySection::Input,
                               VNX::PropertySection::Send, VNX::PropertySection::Receive})
        EXPECT_EQ(VNX::inspectorSectionInfo(section).category, VNX::PropertyCategory::Route);
}

TEST(RoutingInspectorSchema, ReadOnlyListDrawerHidesMutationsAndDisablesEditors)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* drawer = VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
    ASSERT_NE(drawer, nullptr);

    const auto* receives = findField(VNX::busInspectorSchema(), QLatin1String("receives"));
    ASSERT_NE(receives, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(*receives, nullptr));
    drawer->bind(w.get(),
                 QVariantList{QVariantMap{{QStringLiteral("source"), 7u},
                                          {QStringLiteral("level"), -6.0}}},
                 nullptr);

    EXPECT_EQ(w->findChild<QToolButton*>(QStringLiteral("listAdd_receives")), nullptr);
    EXPECT_TRUE(w->findChildren<QToolButton*>(QStringLiteral("listRemove_receives")).isEmpty());
    EXPECT_TRUE(w->findChildren<QToolButton*>(QStringLiteral("listRemoveSelected_receives")).isEmpty());

    EXPECT_TRUE(w->findChildren<QDoubleSpinBox*>().isEmpty());
    QLabel* levelLabel = nullptr;
    for (auto* label : w->findChildren<QLabel*>())
        if (label->property("roValue").toBool()) levelLabel = label;
    ASSERT_NE(levelLabel, nullptr);
    EXPECT_EQ(levelLabel->text(), QStringLiteral("-6.0"));
    EXPECT_EQ(levelLabel->toolTip(), QStringLiteral("-6.0 dB"));

    auto* ref = w->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(ref, nullptr);
    auto* pick = ref->findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    EXPECT_TRUE(pick->isHidden());
}

TEST(RoutingInspectorSchema, EditableSendsDrawerKeepsMutations)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* drawer = VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
    ASSERT_NE(drawer, nullptr);

    const auto* sends = findField(VNX::busInspectorSchema(), QLatin1String("sends"));
    ASSERT_NE(sends, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(*sends, nullptr));
    drawer->bind(w.get(),
                 QVariantList{QVariantMap{{QStringLiteral("targetBusID"), 5u},
                                          {QStringLiteral("sendLevel"), 0.0}}},
                 nullptr);

    auto* addBtn = w->findChild<QToolButton*>(QStringLiteral("listAdd_sends"));
    ASSERT_NE(addBtn, nullptr);
    EXPECT_FALSE(addBtn->isHidden());
    EXPECT_TRUE(w->findChildren<QToolButton*>(QStringLiteral("listRemove_sends")).isEmpty());
    ASSERT_NE(w->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_sends")), nullptr);

    const auto spins = w->findChildren<QDoubleSpinBox*>();
    ASSERT_EQ(spins.size(), 1);
    EXPECT_TRUE(spins[0]->isEnabled());
    EXPECT_TRUE(spins[0]->suffix().isEmpty());
    EXPECT_EQ(spins[0]->toolTip(), QStringLiteral("0.0 dB"));
}

namespace {

using namespace VNX;

class RoutingInspectorInputsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        registerBuiltinInspectorCustomizations();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("RoutingInspectorInputs");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        master = manager.getMasterBusItem();
        b1 = create("B1", ProjectItemType::Bus, master);
        b2 = create("B2", ProjectItemType::Bus, master);
        container = create("C1", ProjectItemType::Container);
        s1 = create("S1", ProjectItemType::Sound, container);
        s2 = create("S2", ProjectItemType::Sound, container);
        container->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -6.0f;
        ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectData& project()
    {
        return *ProjectManager::instance().currentProject();
    }

    ProjectItem* create(const char* name, ProjectItemType type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        if (!item) throw std::runtime_error("Cannot create routing test object");
        if (type != ProjectItemType::Bus) sources.push_back({item->guid, {}});
        return item;
    }

    void expectRows(ProjectItem* bus, const char* field, const std::map<uint32_t, float>& expected)
    {
        const auto* descriptor = findField(busInspectorSchema(), QLatin1String(field));
        ASSERT_NE(descriptor, nullptr);
        const auto rows = descriptor->get(bus->comp<VBus>()).toList();
        ASSERT_EQ(rows.size(), expected.size());
        std::set<uint32_t> seen;
        for (const auto& row : rows) {
            const auto values = row.toMap();
            const auto source = values.value(QStringLiteral("source")).toUInt();
            EXPECT_TRUE(seen.insert(source).second);
            const auto found = expected.find(source);
            ASSERT_NE(found, expected.end());
            EXPECT_NEAR(values.value(QStringLiteral("level")).toDouble(), found->second, 1e-4);
        }
        const auto scope = toFieldGroupScope(currentWorldPlatformScope(
            PlatformContextService::instance().context()));
        const auto graph = buildSignalPathGraph(project(), sources, scope, {master->objectID});
        ASSERT_TRUE(graph.ok());
        const auto kind = QLatin1String(field) == QLatin1String("inputs")
            ? SignalPathEdgeKind::Output : SignalPathEdgeKind::Send;
        size_t count = 0;
        for (const auto& edge : graph.edges) {
            if (edge.kind == kind
                && graph.nodes[edge.to.occurrenceIndex].objectID == bus->objectID) {
                ASSERT_TRUE(edge.routingSource);
                const auto* source = ProjectManager::instance().findItemByGuid(edge.routingSource->objectGuid);
                ASSERT_NE(source, nullptr);
                EXPECT_TRUE(expected.contains(source->objectID));
                ++count;
            }
        }
        EXPECT_EQ(count, rows.size());
    }

    void expectDisplayedSources(InspectorPanel& inspector, const char* field,
                                const std::set<uint32_t>& expected)
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* list = inspector.findChild<QWidget*>(QStringLiteral("prop_") + QLatin1String(field));
        ASSERT_NE(list, nullptr);
        std::set<uint32_t> actual;
        const auto references = list->findChildren<ObjectRefWidget*>();
        for (const auto* reference : references) actual.insert(reference->currentObjectID());
        EXPECT_EQ(references.size(), expected.size());
        EXPECT_EQ(actual, expected);
    }

    QTemporaryDir directory;
    std::vector<SignalPathIdentity> sources;
    ProjectItem* master = nullptr;
    ProjectItem* b1 = nullptr;
    ProjectItem* b2 = nullptr;
    ProjectItem* container = nullptr;
    ProjectItem* s1 = nullptr;
    ProjectItem* s2 = nullptr;
};

}

TEST_F(RoutingInspectorInputsTest, MainInputCountsConfigurationsInsteadOfInheritingObjects)
{
    EXPECT_EQ(container->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    for (const auto* child : {s1, s2}) {
        EXPECT_EQ(child->comp<VMixNode>()->outputBusId, master->objectID);
        EXPECT_EQ(child->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    }
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f},
                                     {container->objectID, -6.0f}});
    auto* nested = create("Nested", ProjectItemType::Container, container);
    create("NestedSound", ProjectItemType::Sound, nested);
    s1->fieldGroups.mainOutput.shared = {FieldGroupMode::Inherit,
        MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -18.0f}};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f},
                                     {container->objectID, -6.0f}});
    expectRows(b1, "inputs", {});
}

TEST_F(RoutingInspectorInputsTest, MainInputKeepsSeparateLocalConfigurationsToTheSameBus)
{
    s1->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(master->guid), -6.0f}};
    s2->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(b2->guid), -12.0f}};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f},
                                     {container->objectID, -6.0f}, {s1->objectID, -6.0f}});
    expectRows(b2, "inputs", {{s2->objectID, -12.0f}});
    s1->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Inherit;
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f},
                                     {container->objectID, -6.0f}});
}

TEST_F(RoutingInspectorInputsTest, ReceiveCountsInheritedSendOnceAndKeepsIndependentSources)
{
    container->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b1->guid), -9.0f}}}};
    b2->comp<VMixNode>()->sends = {{b1->objectID, VMixNode::dbToLinear(-3.0f)}};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    for (const auto* child : {s1, s2}) ASSERT_EQ(child->comp<VMixNode>()->sends.size(), 1u);
    expectRows(b1, "receives", {{container->objectID, -9.0f}, {b2->objectID, -3.0f}});
    s1->fieldGroups.send.shared = container->fieldGroups.send.shared;
    s2->fieldGroups.send.shared = {FieldGroupMode::Inherit,
        SendFieldGroup{{{toFieldGroupGuid(b2->guid), -15.0f}}}};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(b1, "receives", {{container->objectID, -9.0f}, {s1->objectID, -9.0f},
                               {b2->objectID, -3.0f}});
    expectRows(b2, "receives", {});
    EXPECT_TRUE(ProjectManager::instance().busHasIncomingSends(b1->objectID));
    EXPECT_FALSE(ProjectManager::instance().busHasIncomingSends(b2->objectID));
    s1->fieldGroups.send.shared = {FieldGroupMode::Local, SendFieldGroup{}};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(b1, "receives", {{container->objectID, -9.0f}, {b2->objectID, -3.0f}});
}

TEST_F(RoutingInspectorInputsTest, ListsUseCurrentPlatformSourcesAndLevels)
{
    container->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b1->guid), -9.0f}}}};
    container->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -15.0f}};
    container->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(b2->guid), -18.0f}}}};
    s1->fieldGroups.mainOutput.platforms = container->fieldGroups.mainOutput.platforms;
    s1->fieldGroups.send.platforms = container->fieldGroups.send.platforms;
    ASSERT_TRUE(refreshFieldGroupDerivedViews(project()));
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f},
                                     {container->objectID, -6.0f}});
    expectRows(b1, "receives", {{container->objectID, -9.0f}});
    InspectorPanel inputs;
    inputs.showBusInspector(b1->objectID);
    InspectorPanel receives;
    receives.showBusInspector(b2->objectID);
    expectDisplayedSources(inputs, "inputs", {});
    expectDisplayedSources(receives, "receives", {});
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    expectRows(master, "inputs", {{b1->objectID, 0.0f}, {b2->objectID, 0.0f}});
    expectRows(b1, "inputs", {{container->objectID, -15.0f}, {s1->objectID, -15.0f}});
    expectRows(b1, "receives", {});
    expectRows(b2, "receives", {{container->objectID, -18.0f}, {s1->objectID, -18.0f}});
    expectDisplayedSources(inputs, "inputs", {container->objectID, s1->objectID});
    expectDisplayedSources(receives, "receives", {container->objectID, s1->objectID});
    EXPECT_FALSE(ProjectManager::instance().busHasIncomingSends(b1->objectID));
    EXPECT_TRUE(ProjectManager::instance().busHasIncomingSends(b2->objectID));
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
    expectDisplayedSources(inputs, "inputs", {});
    expectDisplayedSources(receives, "receives", {});
}

TEST_F(RoutingInspectorInputsTest, OpenListsRefreshWhenSourcesChangeInheritanceOrRouting)
{
    auto& manager = ProjectManager::instance();
    auto groups = container->fieldGroups;
    groups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b1->guid), -9.0f}}}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(container, groups));
    InspectorPanel inputs;
    inputs.showBusInspector(master->objectID);
    InspectorPanel receives;
    receives.showBusInspector(b1->objectID);
    expectDisplayedSources(inputs, "inputs", {b1->objectID, b2->objectID, container->objectID});
    expectDisplayedSources(receives, "receives", {container->objectID});
    const auto inherited = s1->fieldGroups;
    groups = inherited;
    groups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(master->guid), -6.0f}};
    groups.send.shared = container->fieldGroups.send.shared;
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(s1, groups));
    expectDisplayedSources(inputs, "inputs", {b1->objectID, b2->objectID, container->objectID, s1->objectID});
    expectDisplayedSources(receives, "receives", {container->objectID, s1->objectID});
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(s1, inherited));
    expectDisplayedSources(inputs, "inputs", {b1->objectID, b2->objectID, container->objectID});
    expectDisplayedSources(receives, "receives", {container->objectID});
    groups = container->fieldGroups;
    groups.mainOutput.shared.localHistory->mainOutputBusRef = toFieldGroupGuid(b2->guid);
    groups.send.shared = {FieldGroupMode::Local, SendFieldGroup{}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(container, groups));
    expectDisplayedSources(inputs, "inputs", {b1->objectID, b2->objectID});
    expectDisplayedSources(receives, "receives", {});
    auto* rootSound = create("RootSound", ProjectItemType::Sound);
    expectDisplayedSources(inputs, "inputs", {b1->objectID, b2->objectID, rootSound->objectID});
}
