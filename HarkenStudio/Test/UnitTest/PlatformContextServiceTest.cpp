#include "Support/FieldGroupTestUtils.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContext.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <memory>
#include <optional>
#include <utility>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using VNX::Platform;
using VNX::PlatformContext;
using VNX::PlatformContextService;

QStringList names(std::initializer_list<const char*> raw)
{
    QStringList out;
    for (const char* name : raw)
        out.append(QString::fromLatin1(name));
    return out;
}

QStringList ids(std::initializer_list<const char*> raw)
{
    return HarkenTest::platformIdStrings(names(raw));
}

std::unique_ptr<VNX::MemorySettingsStorage> storageWith(
    const QStringList& selected,
    const QString& primary,
    std::optional<bool> all = std::nullopt)
{
    auto copy = std::make_unique<VNX::MemorySettingsStorage>();
    copy->beginGroup(QStringLiteral("PlatformContext/%1").arg(VNX::ProjectManager::instance().currentProject()->projectId));
    copy->setValue(QStringLiteral("selectedIds"), selected);
    copy->setValue(QStringLiteral("primaryId"), primary);
    if (all.has_value())
        copy->setValue(QStringLiteral("all"), *all);
    copy->endGroup();
    return copy;
}

TEST(PlatformContext, FirstOpenSelectsAllTargetsAndFirstPrimary)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        {}, {}, ids({"Windows", "Linux", "Android"}));
    ASSERT_EQ(context.selected.size(), 3);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.selected.at(1), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(context.selected.at(2), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, SubsetCollapsesToPrimary)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Linux", "Android"}),
        HarkenTest::platformIdString(QStringLiteral("Android")),
        ids({"Windows", "Linux", "Android"}));
    ASSERT_EQ(context.selected.size(), 1);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Android);
    EXPECT_FALSE(context.all);
}

TEST(PlatformContext, EmptyIntersectionFallsBackToAllTargets)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"iOS"}),
        HarkenTest::platformIdString(QStringLiteral("iOS")),
        ids({"Windows", "Linux"}));
    ASSERT_EQ(context.selected.size(), 2);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.selected.at(1), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, StickyAllSurvivesTargetGrowth)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Windows", "Linux"}),
        HarkenTest::platformIdString(QStringLiteral("Linux")),
        ids({"Windows", "Linux", "Android"}),
        true);
    ASSERT_EQ(context.selected.size(), 3);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.selected.at(1), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(context.selected.at(2), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Linux);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, StickyAllKeepsPrimaryWhenTargetShrinks)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Windows", "Linux", "Android"}),
        HarkenTest::platformIdString(QStringLiteral("Linux")),
        ids({"Windows", "Linux"}),
        true);
    ASSERT_EQ(context.selected.size(), 2);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.selected.at(1), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Linux);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, TargetShrinkDropsMissingPrimary)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Windows", "Linux", "Android"}),
        HarkenTest::platformIdString(QStringLiteral("Linux")),
        ids({"Windows"}));
    ASSERT_EQ(context.selected.size(), 1);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, MissingPrimaryFallsBackToFirstSelected)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Windows"}),
        HarkenTest::platformIdString(QStringLiteral("Linux")),
        ids({"Windows", "Android"}),
        false);
    ASSERT_EQ(context.selected.size(), 1);
    EXPECT_EQ(context.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_FALSE(context.all);
}

TEST(PlatformContext, FullSelectionPredicateMatchesAllTargets)
{
    const QStringList targets = ids({"Windows", "Linux", "Android"});
    const PlatformContext all = VNX::resolvePlatformContext({}, {}, targets);
    EXPECT_TRUE(VNX::selectedEqualsTargetPlatforms(all.selected, targets));

    QVector<VNX::ProjectPlatformId> subset = all.selected;
    subset.removeLast();
    EXPECT_FALSE(VNX::selectedEqualsTargetPlatforms(subset, targets));
}

TEST(PlatformContext, OldNamesAndMalformedPreferencesUseValidDefaults)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        names({"Universal", "Android", "invalid-id"}), QStringLiteral("Android"),
        ids({"Windows", "Android"}));
    EXPECT_EQ(context.selected, (QVector<VNX::ProjectPlatformId>{
        HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}));
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(context.all);
}

TEST(PlatformContext, EmptyTargetsYieldEmptyContext)
{
    const PlatformContext context = VNX::resolvePlatformContext(
        ids({"Windows"}), HarkenTest::platformIdString(QStringLiteral("Windows")), {});
    EXPECT_TRUE(context.selected.isEmpty());
    EXPECT_EQ(context.primary, HarkenTest::PlatformIds::Universal);
    EXPECT_FALSE(context.all);
}

TEST(PlatformContextService, ClickedPlatformBecomesPrimary)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextClick");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(service.context().all);
    service.setSelected(service.context().selected, HarkenTest::PlatformIds::Android);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    EXPECT_EQ(service.context().selected.size(), 3);
    EXPECT_TRUE(service.context().all);

    service.setSelected(QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Linux}, HarkenTest::PlatformIds::Linux, false);
    ASSERT_EQ(service.context().selected.size(), 1);
    EXPECT_EQ(service.context().selected.at(0), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Linux);
    EXPECT_FALSE(service.context().all);

    pm.closeProject();
}

TEST(PlatformContextService, PersistenceRoundTripThroughInjectedStorage)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextPersist");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    VNX::MemorySettingsStorage* raw = storage.get();
    QStringList persistedSelected;
    QString persistedPrimary;
    bool persistedAll = false;
    {
        PlatformContextService first(std::move(storage));
        first.setSelected(QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Linux}, HarkenTest::PlatformIds::Linux, false);
        raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(VNX::ProjectManager::instance().currentProject()->projectId));
        persistedSelected = raw->value(QStringLiteral("selectedIds")).toStringList();
        persistedPrimary = raw->value(QStringLiteral("primaryId")).toString();
        persistedAll = raw->value(QStringLiteral("all")).toBool();
        raw->endGroup();
        EXPECT_EQ(persistedSelected, ids({"Linux"}));
        EXPECT_EQ(persistedPrimary, HarkenTest::platformIdString(QStringLiteral("Linux")));
        EXPECT_FALSE(persistedAll);
    }

    PlatformContextService second(
        storageWith(persistedSelected, persistedPrimary, persistedAll));
    ASSERT_EQ(second.context().selected.size(), 1);
    EXPECT_EQ(second.context().selected.at(0), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(second.context().primary, HarkenTest::PlatformIds::Linux);
    EXPECT_FALSE(second.context().all);

    pm.closeProject();
}

TEST(PlatformContextService, StickyAllGrowsWithNewTarget)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextStickyAll");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux"}));
    ASSERT_TRUE(pm.createProject(config));

    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    service.setSelected(
        QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Linux},
        HarkenTest::PlatformIds::Linux,
        true);
    EXPECT_TRUE(service.context().all);

    int emissions = 0;
    PlatformContext observed;
    QObject::connect(&service, &PlatformContextService::contextChanged,
                     &service, [&](const PlatformContext& context) {
                         ++emissions;
                         observed = context;
                     });

    pm.setProjectPlatformsInternal(HarkenTest::platforms(names({"Windows", "Linux", "Android"})));
    EXPECT_EQ(emissions, 1);
    ASSERT_EQ(observed.selected.size(), 3);
    EXPECT_EQ(observed.selected.at(2), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(observed.primary, HarkenTest::PlatformIds::Linux);
    EXPECT_TRUE(observed.all);

    pm.closeProject();
}

TEST(PlatformContextService, AllPersistsAcrossTargetsAndReopeningWithoutChangingPrimary)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextAll");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    auto* raw = storage.get();
    const QString configProjectId = pm.currentProject()->projectId;
    PlatformContextService service(std::move(storage));
    service.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    int emissions = 0;
    QObject::connect(&service, &PlatformContextService::contextChanged,
                     &service, [&](const PlatformContext&) { ++emissions; });

    service.setAll(true);
    EXPECT_EQ(emissions, 1);
    EXPECT_EQ(service.context().selected, VNX::occupiedTargetPlatforms(VNX::ProjectPlatformSemantics::ids(config.platforms)));
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    service.setAll(true);
    EXPECT_EQ(emissions, 1);

    pm.setProjectPlatformsInternal(HarkenTest::platforms(names({"Windows", "Linux", "Android"})));
    EXPECT_EQ(emissions, 2);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().selected.size(), 3);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);

    raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(configProjectId));
    const QStringList selected = raw->value(QStringLiteral("selectedIds")).toStringList();
    const QString primary = raw->value(QStringLiteral("primaryId")).toString();
    const bool all = raw->value(QStringLiteral("all")).toBool();
    raw->endGroup();
    EXPECT_EQ(selected, ids({"Windows", "Linux", "Android"}));
    EXPECT_EQ(primary, HarkenTest::platformIdString("Android"));
    EXPECT_TRUE(all);

    PlatformContextService reopened(storageWith(selected, primary, all));
    EXPECT_EQ(reopened.context(), service.context());

    pm.closeProject();
    EXPECT_EQ(emissions, 3);
    EXPECT_EQ(service.context(), PlatformContext{});
    raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(configProjectId));
    EXPECT_TRUE(raw->value(QStringLiteral("all")).toBool());
    raw->endGroup();
}

TEST(PlatformContextService, ChangingSelectionDoesNotToggleExplicitAll)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextExplicitAll");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    auto* raw = storage.get();
    const QString configProjectId = pm.currentProject()->projectId;
    PlatformContextService service(std::move(storage));
    service.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().selected, VNX::occupiedTargetPlatforms(VNX::ProjectPlatformSemantics::ids(config.platforms)));
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    service.setAll(false);
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Android});
    EXPECT_EQ(VNX::currentWorldPlatformScope(service.context()), HarkenTest::PlatformIds::Android);
    service.setSelected({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows);
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Windows});
    raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(configProjectId));
    EXPECT_FALSE(raw->value(QStringLiteral("all")).toBool());
    raw->endGroup();

    service.setAll(true);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().selected,
              VNX::occupiedTargetPlatforms(VNX::ProjectPlatformSemantics::ids(config.platforms)));
    EXPECT_EQ(VNX::currentWorldPlatformScope(service.context()), HarkenTest::PlatformIds::Windows);
    pm.closeProject();
}

TEST(PlatformContextService, TargetPlatformsChangedRecomputesAndEmits)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextTargets");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    service.setSelected(QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Linux}, HarkenTest::PlatformIds::Linux, false);

    int emissions = 0;
    PlatformContext observed;
    QObject::connect(&service, &PlatformContextService::contextChanged,
                     &service, [&](const PlatformContext& context) {
                         ++emissions;
                         observed = context;
                     });

    pm.setProjectPlatformsInternal(HarkenTest::platforms(names({"Windows"})));
    EXPECT_EQ(emissions, 1);
    ASSERT_EQ(observed.selected.size(), 1);
    EXPECT_EQ(observed.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(observed.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(observed.all);
    EXPECT_EQ(service.context().selected, observed.selected);

    pm.closeProject();
}

TEST(PlatformContextService, SingleTargetKeepsExplicitPlatformModeAcrossReopening)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextSingleTarget");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows"}));
    ASSERT_TRUE(pm.createProject(config));
    auto storage = storageWith(ids({"Windows"}), HarkenTest::platformIdString("Windows"), false);
    auto* raw = storage.get();
    const QString configProjectId = pm.currentProject()->projectId;
    raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(configProjectId));
    raw->endGroup();
    PlatformContextService service(std::move(storage));
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Windows});
    raw->beginGroup(QStringLiteral("PlatformContext/%1").arg(configProjectId));
    raw->endGroup();
    service.setAll(true);
    EXPECT_TRUE(service.context().all);
    service.setAll(false);
    EXPECT_FALSE(service.context().all);
    const auto before = service.context();
    const QString path = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(path));
    EXPECT_EQ(service.context(), before);
    EXPECT_FALSE(service.context().all);
    pm.closeProject();
}

TEST(PlatformContextService, EmptyTargetsYieldEmptyContext)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_TRUE(service.context().selected.isEmpty());
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Universal);
    EXPECT_FALSE(service.context().all);
}

TEST(PlatformContextService, SingleWindowsSupportsBothAllAndIndependentEditing)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_EQ(service.context(), PlatformContext{});
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SingleWindowsContext");
    config.location = directory.path();
    config.platforms = HarkenTest::platforms(names({"Windows"}));
    ASSERT_TRUE(pm.createProject(config));
    ASSERT_EQ(pm.currentProject()->platforms.size(), 1);
    const auto id = pm.currentProject()->platforms.front().id;
    EXPECT_EQ(pm.currentProject()->platforms.front().basePlatform, VNX::BasePlatform::Windows);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{id});
    service.setAll(false);
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(VNX::currentWorldPlatformScope(service.context()), id);
    service.setSelected({id}, id);
    EXPECT_FALSE(service.context().all);
    service.setAll(true);
    EXPECT_TRUE(service.context().all);
    pm.closeProject();
    EXPECT_EQ(service.context(), PlatformContext{});
}

TEST(PlatformContextService, ProjectOpenedRecomputesAndEmits)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();

    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_TRUE(service.context().selected.isEmpty());
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Universal);

    int emissions = 0;
    PlatformContext observed;
    QObject::connect(&service, &PlatformContextService::contextChanged,
                     &service, [&](const PlatformContext& context) {
                         ++emissions;
                         observed = context;
                     });

    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextOpened");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    EXPECT_EQ(emissions, 1);
    ASSERT_EQ(observed.selected.size(), 3);
    EXPECT_EQ(observed.selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(observed.selected.at(1), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(observed.selected.at(2), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(observed.primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(observed.all);
    EXPECT_EQ(service.context().selected, observed.selected);
    EXPECT_EQ(service.context().primary, observed.primary);

    pm.closeProject();
}

TEST(PlatformContextService, ProjectClosedYieldsEmptyContextAndEmits)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformContextClosed");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux"}));
    ASSERT_TRUE(pm.createProject(config));

    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    ASSERT_EQ(service.context().selected.size(), 2);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(service.context().all);

    int emissions = 0;
    PlatformContext observed;
    QObject::connect(&service, &PlatformContextService::contextChanged,
                     &service, [&](const PlatformContext& context) {
                         ++emissions;
                         observed = context;
                     });

    pm.closeProject();
    EXPECT_EQ(emissions, 1);
    EXPECT_TRUE(observed.selected.isEmpty());
    EXPECT_EQ(observed.primary, HarkenTest::PlatformIds::Universal);
    EXPECT_FALSE(observed.all);
    EXPECT_TRUE(service.context().selected.isEmpty());
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Universal);
}

TEST(PlatformContextService, ProjectsWithTheSamePlatformIdsKeepIndependentPreferences)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir directory;
    PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::ProjectConfig config;
    config.location = directory.path();
    config.name = "ProjectA";
    config.platforms = HarkenTest::platforms(names({"Windows", "Android"}));
    ASSERT_TRUE(pm.createProject(config));
    const auto a = pm.currentProject()->projectPath;
    service.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    EXPECT_TRUE(service.context().selected.isEmpty());
    config.name = "ProjectB";
    ASSERT_TRUE(pm.createProject(config));
    const auto b = pm.currentProject()->projectPath;
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    service.setSelected(service.context().selected, HarkenTest::PlatformIds::Windows, true);
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(a));
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    auto platforms = pm.currentProject()->platforms;
    platforms.last().name = "Android_Renamed";
    pm.setProjectPlatformsInternal(platforms);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(b));
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    pm.closeProject();
}

TEST(PlatformContextService, OldGlobalPreferencesAreIgnoredEvenWhenIdsMatch)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    storage->beginGroup("PlatformContext");
    storage->setValue("selected", ids({"Android"}));
    storage->setValue("primary", HarkenTest::platformIdString("Android"));
    storage->setValue("all", false);
    storage->endGroup();
    PlatformContextService service(std::move(storage));
    QTemporaryDir directory;
    VNX::ProjectConfig config;
    config.location = directory.path();
    config.name = "IgnoreLegacy";
    config.platforms = HarkenTest::platforms(names({"Windows", "Android"}));
    ASSERT_TRUE(pm.createProject(config));
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    pm.closeProject();
}

}
