
#include "BindingEditing.h"
#include "Components/VAction.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "VariableConsumerSemantics.h"
#include "VariableImpactSemantics.h"
#include "VariableSemantics.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

class VariableImpactSemanticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("VariableImpact");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        sound_ = manager.createItem(nullptr, QStringLiteral("HostSound"),
                                    VNX::ProjectItem::Type::Sound);
        container_ = manager.createItem(nullptr, QStringLiteral("HostContainer"),
                                        VNX::ProjectItem::Type::Container);
        child_ = manager.createItem(container_, QStringLiteral("Child"),
                                    VNX::ProjectItem::Type::Sound);
        drive_ = manager.createItem(nullptr, QStringLiteral("Drive"),
                                    VNX::ProjectItem::Type::Variable);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(child_, nullptr);
        ASSERT_NE(drive_, nullptr);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
    }

    void makeDriveDiscrete()
    {
        auto* variable = drive_->comp<VNX::VVariable>();
        ASSERT_NE(variable, nullptr);
        variable->kind = VNX::VariableKind::Step;
        variable->rangeMin = 0.0f;
        variable->rangeMax = 2.0f;
        variable->defaultValue = 2.0f;
        variable->labels = {{0, QStringLiteral("off")}};
    }

    bool attachSetValueGridAndBinding()
    {
        auto* selection = container_->compOrCreate<VNX::VSelection>();
        if (!selection)
            return false;
        selection->mode = VNX::SelectionMode::Match;
        auto grid = selection->matchGrid();
        if (!grid.addAxis(drive_->guid))
            return false;
        if (!grid.addRow(child_->guid))
            return false;
        if (!grid.setCell(0, 0, 2))
            return false;
        if (!selection->selections.shared.setMatchGrid(grid))
            return false;
        auto binding = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), drive_->guid);
        if (!VNX::BindingEditing::upsert(sound_, binding))
            return false;
        auto& manager = VNX::ProjectManager::instance();
        action_ = manager.createItem(nullptr, QStringLiteral("SetDrive"),
                                     VNX::ProjectItem::Type::Action);
        if (!action_)
            return false;
        auto* component = action_->comp<VNX::VAction>();
        if (!component)
            return false;
        VNX::VActionStep setValue;
        setValue.type = VNX::VActionStep::Type::SetVariable;
        setValue.targetObjectID = drive_->objectID;
        setValue.value = 2.0f;
        component->steps = {setValue};
        return true;
    }

    static int countInvalid(const VNX::VariableImpactReport& report,
                            VNX::VariableImpactInvalidKind kind)
    {
        int count = 0;
        for (const auto& invalid : report.invalidValues) {
            if (invalid.kind == kind)
                ++count;
        }
        return count;
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    VNX::ProjectItem* drive_ = nullptr;
    VNX::ProjectItem* action_ = nullptr;
};

TEST_F(VariableImpactSemanticsTest, RangeShrinkListsInvalidDefaultActionAndGridAndResetsBank)
{
    makeDriveDiscrete();
    ASSERT_TRUE(attachSetValueGridAndBinding());
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto proposed = VNX::VariableImpactSemantics::snapshotOf(*drive_);
    proposed.rangeMax = 1.0f;

    const auto report = VNX::VariableImpactSemantics::analyzeChange(
        project, drive_->guid, proposed);

    EXPECT_EQ(countInvalid(report, VNX::VariableImpactInvalidKind::Default), 1);
    EXPECT_EQ(countInvalid(report, VNX::VariableImpactInvalidKind::ActionSetVariable), 1);
    EXPECT_EQ(countInvalid(report, VNX::VariableImpactInvalidKind::DiscreteGridCell), 1);
    for (const auto& invalid : report.invalidValues)
        EXPECT_FLOAT_EQ(invalid.value, 2.0f);
    EXPECT_TRUE(report.executionFingerprintChanged);
    EXPECT_TRUE(report.liveBankResets);
    EXPECT_TRUE(report.packMustRebuild);
    EXPECT_TRUE(report.sdkCatalogMustRebuild);
    bool hasBinding = false;
    bool hasGrid = false;
    bool hasSetValue = false;
    for (const auto& consumer : report.consumers) {
        if (consumer.kind == VNX::VariableConsumerKind::Binding)
            hasBinding = true;
        else if (consumer.kind == VNX::VariableConsumerKind::DiscreteGridAxis)
            hasGrid = true;
        else if (consumer.kind == VNX::VariableConsumerKind::ActionSetVariable)
            hasSetValue = true;
    }
    EXPECT_TRUE(hasBinding);
    EXPECT_TRUE(hasGrid);
    EXPECT_TRUE(hasSetValue);
    EXPECT_FLOAT_EQ(drive_->comp<VNX::VVariable>()->defaultValue, 2.0f);
    EXPECT_FLOAT_EQ(drive_->comp<VNX::VVariable>()->rangeMax, 2.0f);
}

TEST_F(VariableImpactSemanticsTest, UnitOnlyChangeRebuildsSdkNotBank)
{
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto proposed = VNX::VariableImpactSemantics::snapshotOf(*drive_);
    proposed.unit = QStringLiteral("dB");

    const auto report = VNX::VariableImpactSemantics::analyzeChange(
        project, drive_->guid, proposed);

    EXPECT_EQ(report.currentExecutionFingerprint, report.proposedExecutionFingerprint);
    EXPECT_FALSE(report.executionFingerprintChanged);
    EXPECT_FALSE(report.liveBankResets);
    EXPECT_TRUE(report.packMustRebuild);
    EXPECT_TRUE(report.catalogFingerprintChanged);
    EXPECT_TRUE(report.sdkCatalogMustRebuild);
    EXPECT_TRUE(report.invalidValues.empty());
    EXPECT_EQ(drive_->comp<VNX::VVariable>()->unit, QString());
}

TEST_F(VariableImpactSemanticsTest, LabelWordingRebuildsCatalogOnly)
{
    makeDriveDiscrete();
    drive_->comp<VNX::VVariable>()->defaultValue = 0.0f;
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto proposed = VNX::VariableImpactSemantics::snapshotOf(*drive_);
    ASSERT_FALSE(proposed.labels.empty());
    proposed.labels.front().name = QStringLiteral("closed");

    const auto report = VNX::VariableImpactSemantics::analyzeChange(
        project, drive_->guid, proposed);

    EXPECT_EQ(report.currentExecutionFingerprint, report.proposedExecutionFingerprint);
    EXPECT_FALSE(report.executionFingerprintChanged);
    EXPECT_FALSE(report.liveBankResets);
    EXPECT_TRUE(report.packMustRebuild);
    EXPECT_TRUE(report.catalogFingerprintChanged);
    EXPECT_TRUE(report.sdkCatalogMustRebuild);
    EXPECT_EQ(drive_->comp<VNX::VVariable>()->labels.front().name,
              QStringLiteral("off"));
}

TEST_F(VariableImpactSemanticsTest, LabelAnchorChangeResetsBank)
{
    makeDriveDiscrete();
    drive_->comp<VNX::VVariable>()->defaultValue = 0.0f;
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto proposed = VNX::VariableImpactSemantics::snapshotOf(*drive_);
    ASSERT_FALSE(proposed.labels.empty());
    proposed.labels.front().anchor = 1;

    const auto report = VNX::VariableImpactSemantics::analyzeChange(
        project, drive_->guid, proposed);

    EXPECT_TRUE(report.executionFingerprintChanged);
    EXPECT_TRUE(report.liveBankResets);
    EXPECT_TRUE(report.packMustRebuild);
    EXPECT_TRUE(report.sdkCatalogMustRebuild);
    EXPECT_EQ(drive_->comp<VNX::VVariable>()->labels.front().anchor, 0);
}

TEST_F(VariableImpactSemanticsTest, ConsumersComeFromTheSameGraph)
{
    makeDriveDiscrete();
    ASSERT_TRUE(attachSetValueGridAndBinding());
    auto* blend = VNX::ProjectManager::instance().createItem(
        container_, QStringLiteral("Blend"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(blend, nullptr);
    auto* blendSelection = blend->comp<VNX::VSelection>();
    ASSERT_NE(blendSelection, nullptr);
    blendSelection->mode = VNX::SelectionMode::Blend;
    blendSelection->selections.shared.mixingRows.push_back({QUuid::createUuid(), drive_->guid});
    auto* project = VNX::ProjectManager::instance().currentProject();
    const auto graph = VNX::VariableConsumerSemantics::consumersOf(
        project, drive_->guid);
    const auto report = VNX::VariableImpactSemantics::analyzeChange(
        project, drive_->guid, VNX::VariableImpactSemantics::snapshotOf(*drive_));
    ASSERT_EQ(report.consumers.size(), graph.size());
    ASSERT_EQ(graph.size(), 4u);
    for (size_t i = 0; i < graph.size(); ++i) {
        EXPECT_EQ(report.consumers[i].kind, graph[i].kind);
        EXPECT_EQ(report.consumers[i].hostGuid, graph[i].hostGuid);
        EXPECT_EQ(report.consumers[i].bindingStableId, graph[i].bindingStableId);
        EXPECT_EQ(report.consumers[i].axisIndex, graph[i].axisIndex);
        EXPECT_EQ(report.consumers[i].address, graph[i].address);
    }
}

TEST_F(VariableImpactSemanticsTest, LabelPermutationChangesMappingWithoutResettingNumericBank)
{
    makeDriveDiscrete();
    auto* variable = drive_->comp<VNX::VVariable>();
    variable->labels = {{1, QStringLiteral("AAA")}, {2, QStringLiteral("BBB")},
                        {10, QStringLiteral("Hidden")}};
    ASSERT_TRUE(attachSetValueGridAndBinding());
    auto* selection = container_->comp<VNX::VSelection>();
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.setCell(0, 0, variable->labels[0].id));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    auto proposed = VNX::VariableImpactSemantics::snapshotOf(*drive_);
    auto* project = VNX::ProjectManager::instance().currentProject();
    proposed.labels[2].name = QStringLiteral("HiddenRename");
    const auto hidden = VNX::VariableImpactSemantics::analyzeChange(project, drive_->guid, proposed);
    EXPECT_FALSE(hidden.packMustRebuild);
    EXPECT_FALSE(hidden.sdkCatalogMustRebuild);
    std::swap(proposed.labels[0].anchor, proposed.labels[1].anchor);
    const auto moved = VNX::VariableImpactSemantics::analyzeChange(project, drive_->guid, proposed);
    EXPECT_FALSE(moved.liveBankResets);
    EXPECT_FALSE(moved.executionFingerprintChanged);
    EXPECT_TRUE(moved.labelMappingChanged);
    EXPECT_TRUE(moved.packMustRebuild);
    proposed.labels[0].anchor = 11;
    const auto excluded = VNX::VariableImpactSemantics::analyzeChange(project, drive_->guid, proposed);
    EXPECT_EQ(countInvalid(excluded, VNX::VariableImpactInvalidKind::DiscreteGridCell), 1);
}
