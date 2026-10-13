#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Components/VMixNode.h"
#include "FieldGroupProjectAdapter.h"

#include <gtest/gtest.h>

#include <memory>
#include <QTemporaryDir>

using VNX::ProjectItem;
using VNX::ProjectManager;

namespace {

std::shared_ptr<ProjectItem> makeItem(const QString& name, ProjectItem::Type type)
{
    return std::make_shared<ProjectItem>(name, type);
}

} // namespace

TEST(VolumeAggregation, SumsVoiceVolumeAndMakeUpGainWithoutOutputVolume)
{
    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer);
    layer->compOrCreate<VNX::VMixNode>()->volume = 8.0f;
    layer->comp<VNX::VMixNode>()->outputBusVolume = -16.0f;

    auto sound = makeItem(QStringLiteral("Snd"), ProjectItem::Type::Sound);
    sound->compOrCreate<VNX::VMixNode>()->volume = 1.0f;
    sound->comp<VNX::VMixNode>()->outputBusVolume = 2.0f;
    sound->comp<VNX::VMixNode>()->makeUpGain = 4.0f;
    sound->parent = layer.get();

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(sound.get()), 13.0f);
}

TEST(VolumeAggregation, LayerMakeUpGainEntersTheParentChainSum)
{
    auto outer = makeItem(QStringLiteral("OuterLayer"), ProjectItem::Type::Layer);
    outer->compOrCreate<VNX::VMixNode>()->makeUpGain = 5.0f;

    auto inner = makeItem(QStringLiteral("InnerLayer"), ProjectItem::Type::Layer);
    inner->compOrCreate<VNX::VMixNode>()->volume = -2.0f;
    inner->comp<VNX::VMixNode>()->makeUpGain = 3.0f;
    inner->parent = outer.get();

    auto sound = makeItem(QStringLiteral("Snd"), ProjectItem::Type::Sound);
    sound->compOrCreate<VNX::VMixNode>()->volume = -1.0f;
    sound->parent = inner.get();

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(inner.get()), 6.0f);
    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(sound.get()), 5.0f);
}

TEST(VolumeAggregation, StopsAtBusAncestor)
{
    auto bus = makeItem(QStringLiteral("BusA"), ProjectItem::Type::Bus);
    bus->compOrCreate<VNX::VMixNode>()->volume = -12.0f;
    bus->comp<VNX::VMixNode>()->outputBusVolume = -24.0f;
    bus->comp<VNX::VMixNode>()->makeUpGain = 6.0f;

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer);
    layer->compOrCreate<VNX::VMixNode>()->volume = -3.0f;
    layer->parent = bus.get();

    auto sound = makeItem(QStringLiteral("Snd"), ProjectItem::Type::Sound);
    sound->compOrCreate<VNX::VMixNode>()->volume = -6.0f;
    sound->parent = layer.get();

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(sound.get()), -9.0f);
}

TEST(VolumeAggregation, MissingComponentsContributeZero)
{
    auto folder = makeItem(QStringLiteral("Folder"), ProjectItem::Type::None);

    auto bare = makeItem(QStringLiteral("Bare"), ProjectItem::Type::Sound);
    bare->parent = folder.get();
    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(bare.get()), 0.0f);

    auto mixOnly = makeItem(QStringLiteral("MixOnly"), ProjectItem::Type::Sound);
    mixOnly->compOrCreate<VNX::VMixNode>()->volume = -4.5f;
    mixOnly->parent = folder.get();
    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(mixOnly.get()), -4.5f);

    auto gainOnly = makeItem(QStringLiteral("GainOnly"), ProjectItem::Type::Sound);
    gainOnly->compOrCreate<VNX::VMixNode>()->makeUpGain = 3.0f;
    gainOnly->parent = folder.get();
    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(gainOnly.get()), 3.0f);
}

TEST(VolumeAggregation, AggregatesForLayerFromItselfUpward)
{
    auto outer = makeItem(QStringLiteral("Outer"), ProjectItem::Type::Layer);
    outer->compOrCreate<VNX::VMixNode>()->volume = -2.0f;

    auto inner = makeItem(QStringLiteral("Inner"), ProjectItem::Type::Layer);
    inner->compOrCreate<VNX::VMixNode>()->volume = -1.0f;
    inner->parent = outer.get();

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().aggregateVolumeDbForItem(inner.get()), -3.0f);
}

TEST(VolumeAggregation, EffectiveBusVolumeExcludesOutputVolume)
{
    auto bus = makeItem(QStringLiteral("BusFold"), ProjectItem::Type::Bus);
    bus->compOrCreate<VNX::VMixNode>()->volume = -6.0f;
    bus->comp<VNX::VMixNode>()->outputBusVolume = -3.5f;
    bus->comp<VNX::VMixNode>()->makeUpGain = 2.0f;

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().effectiveBusVolumeDb(bus.get()), -4.0f);
}

TEST(VolumeAggregation, EffectiveBusVolumeWithoutMixNodeIsZero)
{
    auto bus = makeItem(QStringLiteral("BusBare"), ProjectItem::Type::Bus);

    EXPECT_FLOAT_EQ(
        ProjectManager::instance().effectiveBusVolumeDb(bus.get()), 0.0f);
    EXPECT_FLOAT_EQ(
        ProjectManager::instance().effectiveBusVolumeDb(nullptr), 0.0f);
}

TEST(VolumeAggregation, OutputVolumeUsesTheCompleteMainOutputGroup)
{
    auto& manager = ProjectManager::instance();
    if (manager.currentProject())
        manager.closeProject();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("OutputVolumeGroups");
    config.location = directory.path();
    ASSERT_TRUE(manager.createProject(config));
    auto* parent = manager.createItem(nullptr, QStringLiteral("Parent"), ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    auto* child = manager.createItem(parent, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Output"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    auto parentGroups = parent->fieldGroups;
    parentGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(manager.getMasterBusItem()->guid), -12.0f}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(parent, parentGroups));
    EXPECT_FLOAT_EQ(-12.0f, manager.effectiveOutputVolumeDbForItem(child));
    auto childGroups = child->fieldGroups;
    childGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(bus->guid), 0.0f}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(child, childGroups));
    EXPECT_FLOAT_EQ(0.0f, manager.effectiveOutputVolumeDbForItem(child));
    childGroups.mainOutput.shared.localHistory->outputBusVolume = -3.0f;
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(child, childGroups));
    EXPECT_FLOAT_EQ(-3.0f, manager.effectiveOutputVolumeDbForItem(child));
    childGroups.mainOutput.shared.mode = VNX::FieldGroupMode::Inherit;
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(child, childGroups));
    EXPECT_FLOAT_EQ(-12.0f, manager.effectiveOutputVolumeDbForItem(child));
    manager.closeProject();
}
