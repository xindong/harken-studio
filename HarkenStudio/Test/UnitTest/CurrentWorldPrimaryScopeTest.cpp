#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AudioEngine.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "CurrentWorldProjectAdapter.h"
#include "ISettingsStorage.h"
#include "MemorySettingsStorage.h"
#include "PlatformContext.h"
#include "PlatformContextService.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "RuntimeCapabilityMatrix.h"
#include "RuntimeSyncCoordinator.h"
#include "FieldGroupProjectAdapter.h"
#include "TransitionSemantics.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <memory>
#include <utility>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using VNX::Platform;
using VNX::PlatformContext;
using VNX::PlatformContextService;
using VNX::PlatformScope;
using VNX::ProjectData;
using VNX::ProjectItem;
using VNX::RuntimeCapabilityFeature;
using VNX::RuntimeCapabilityOperation;
using VNX::RuntimeCapabilityState;

QStringList names(std::initializer_list<const char*> raw)
{
    QStringList out;
    for (const char* name : raw)
        out.append(QString::fromLatin1(name));
    return out;
}

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

ProjectData makeQueueProject(std::shared_ptr<ProjectItem>* outContainer)
{
    ProjectData project;
    project.nextObjectID = 6;

    auto master = makeItem(QStringLiteral("Master"), ProjectItem::Type::Bus,
                           5, harkentest::StudioObjectId(0x4d4153544552));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(2));
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    auto container = makeItem(QStringLiteral("Queue"), ProjectItem::Type::Container,
                              2, harkentest::StudioObjectId(3));
    auto first = makeItem(QStringLiteral("First"), ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(4));
    auto second = makeItem(QStringLiteral("Second"), ProjectItem::Type::Sound, 4, harkentest::StudioObjectId(5));
    appendChild(layer, container);
    appendChild(container, first);
    appendChild(container, second);
    HarkenTest::setSharedSoundSource(*first, 101);
    first->sourceID = 101;
    HarkenTest::setSharedSoundSource(*second, 102);
    second->sourceID = 102;
    EXPECT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    EXPECT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, second->guid));

    auto* selection = container->comp<VNX::VSelection>();
    EXPECT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Random;
    selection->playType = VNX::SelectionPlayType::Queue;
    selection->behavior.discipline = VNX::RandomDiscipline::Draw;
    selection->selections.shared.find(0)->weight = 1;
    selection->selections.shared.find(1)->weight = 1;

    VNX::ProjectFileBinding binding;
    binding.rootItem = layer;
    project.layers.push_back(std::move(binding));
    if (outContainer != nullptr)
        *outContainer = container;
    return project;
}

} // namespace

namespace VNX {

class CurrentWorldPrimaryScope : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& pm = ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        PlatformContextService::instance().setStorage(
            std::make_unique<MemorySettingsStorage>());
    }

    void TearDown() override
    {
        auto& pm = ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        PlatformContextService::instance().setStorage(
            SettingsStorageFactory::create(
                SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"),
                QStringLiteral("HarkenStudio")));
    }

    void bindEnginePlatformContext()
    {
        AudioEngine::instance().bindCurrentWorldPlatformContext();
    }
};

TEST_F(CurrentWorldPrimaryScope, EmptySelectedYieldsNullopt)
{
    PlatformContext context;
    context.primary = HarkenTest::PlatformIds::Windows;
    EXPECT_EQ(currentWorldPlatformScope(context), std::nullopt);
}

TEST_F(CurrentWorldPrimaryScope, UniversalPrimaryYieldsNullopt)
{
    PlatformContext context;
    context.selected = {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android};
    context.primary = HarkenTest::PlatformIds::Universal;
    EXPECT_EQ(currentWorldPlatformScope(context), std::nullopt);
}

TEST_F(CurrentWorldPrimaryScope, MultiPlatformPrimaryIsTheScope)
{
    PlatformContext context;
    context.selected = {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android};
    context.primary = HarkenTest::PlatformIds::Android;
    const PlatformScope scope = currentWorldPlatformScope(context);
    ASSERT_TRUE(scope.has_value());
    EXPECT_EQ(*scope, HarkenTest::PlatformIds::Android);
}

TEST_F(CurrentWorldPrimaryScope, DefaultWindowsInstanceCompilesCurrentWorldOnLivePath)
{
    auto& pm = ProjectManager::instance();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("UntargetedLiveWorld");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ASSERT_EQ(pm.currentProject()->platforms.size(), 4);
    const auto id = pm.currentProject()->platforms.front().id;
    EXPECT_EQ(pm.currentProject()->platforms.front().basePlatform, BasePlatform::Windows);
    EXPECT_EQ(PlatformContextService::instance().context().selected,
              occupiedTargetPlatforms(pm.getTargetPlatforms()));
    EXPECT_EQ(PlatformContextService::instance().context().primary, id);

    auto* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    QString error;
    const auto request = buildCurrentWorldSyncRequest(
        *project,
        currentWorldPlatformScope(PlatformContextService::instance().context()),
        &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
}

TEST_F(CurrentWorldPrimaryScope, SiblingAndroidPrimaryChangeMarksCurrentWorldMutation)
{
    auto& pm = ProjectManager::instance();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("PrimaryMutation");
    config.location = tempDir.path();
    const auto low = QUuid::createUuid();
    const auto high = QUuid::createUuid();
    config.platforms = {{low, "Android_Low", BasePlatform::Android},
                        {high, "Android_High", BasePlatform::Android}};
    ASSERT_TRUE(pm.createProject(config));

    auto& service = PlatformContextService::instance();
    service.setSelected({low, high}, low, true);
    ASSERT_EQ(service.context().primary, low);

    bindEnginePlatformContext();
    auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
    const auto revisionAtLow = coordinator.currentAuthoringRevision();

    service.setSelected({low, high}, high, true);
    EXPECT_EQ(service.context().primary, high);
    EXPECT_GT(coordinator.currentAuthoringRevision(), revisionAtLow);

    const auto revisionAtHigh = coordinator.currentAuthoringRevision();
    service.setSelected({high}, high, false);
    EXPECT_EQ(service.context().primary, high);
    EXPECT_EQ(coordinator.currentAuthoringRevision(), revisionAtHigh);
}

TEST_F(CurrentWorldPrimaryScope, CapabilityEvaluateUsesPrimaryScopeGuard)
{
    EXPECT_TRUE(PlatformContextService::instance().context().selected.isEmpty());
    EXPECT_EQ(PlatformContextService::instance().context().primary,
              HarkenTest::PlatformIds::Universal);

    std::shared_ptr<ProjectItem> container;
    ProjectData project = makeQueueProject(&container);
    ASSERT_NE(container, nullptr);

    const auto live = RuntimeCapabilityMatrix::evaluate(
        *container, project, RuntimeCapabilityOperation::LiveSync,
        RuntimeCapabilityFeature::FlatContainer);
    EXPECT_EQ(live.state, RuntimeCapabilityState::Ready) << live.diagnostic.toStdString();

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    TransitionRuleDef extra = canonicalTransitionRule();
    extra.source = container->children.front()->guid;
    extra.target = container->children.back()->guid;
    ASSERT_TRUE(selection->transitions.addRule(extra));

    const auto transition = RuntimeCapabilityMatrix::evaluate(
        *container, project, RuntimeCapabilityOperation::AuditionPlay,
        RuntimeCapabilityFeature::Transition);
    EXPECT_NE(transition.state, RuntimeCapabilityState::InvalidAuthoring)
        << transition.diagnostic.toStdString();
    EXPECT_FALSE(transition.diagnostic.contains(
        QStringLiteral("Universal platform is not a compile target")));
}

} // namespace VNX
