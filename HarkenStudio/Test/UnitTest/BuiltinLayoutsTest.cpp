#include "Core/BuiltinLayouts.h"
#include "Core/LayoutController.h"
#include "Core/NamedLayoutStore.h"

#include <QByteArray>

#include <QMenu>

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

class NamedLayoutStoreOverride
{
public:
    explicit NamedLayoutStoreOverride(const QString& path)
    {
        VNX::NamedLayoutStore::setDirPathOverrideForTesting(path);
    }

    ~NamedLayoutStoreOverride()
    {
        VNX::NamedLayoutStore::setDirPathOverrideForTesting(QString());
    }
};

} // namespace

TEST(LayoutController, MigratesRenamedDockObjectNames)
{
    const QByteArray saved =
        "<DockWidgets>"
        "<Widget Name=\"AuditionDock\" Closed=\"0\"/>"
        "<Widget Name=\"MonitorMeterDock\" Closed=\"1\"/>"
        "<Widget Name=\"MeterDock_2\" Closed=\"0\"/>"
        "<Widget Name=\"ExplorerDock\" Closed=\"0\"/>"
        "</DockWidgets>";

    const QByteArray migrated = VNX::migrateDockNames(saved);

    EXPECT_FALSE(migrated.contains("AuditionDock"));
    EXPECT_FALSE(migrated.contains("MonitorMeterDock"));
    EXPECT_TRUE(migrated.contains("Name=\"ControlDock\""));
    EXPECT_TRUE(migrated.contains("Name=\"MeterDock_1\""));
    EXPECT_TRUE(migrated.contains("Name=\"MeterDock_2\""));
    EXPECT_TRUE(migrated.contains("Name=\"ExplorerDock\""));
    EXPECT_EQ(VNX::migrateDockNames(migrated), migrated);

    QByteArray previous = saved;
    previous.replace("AuditionDock", "PreviewDock");
    EXPECT_EQ(VNX::migrateDockNames(previous), migrated);
}
TEST(BuiltinLayouts, FindsIdAndStableSourceName)
{
    ASSERT_NE(VNX::BuiltinLayouts::findByToken(QStringLiteral("default_layout")),
              nullptr);
    ASSERT_NE(VNX::BuiltinLayouts::findByToken(QStringLiteral("DEFAULT_LAYOUT")),
              nullptr);
    ASSERT_NE(VNX::BuiltinLayouts::findByToken(QStringLiteral("Default")),
              nullptr);
    ASSERT_NE(VNX::BuiltinLayouts::findByToken(QStringLiteral("default")),
              nullptr);
    EXPECT_EQ(VNX::BuiltinLayouts::findByToken(QStringLiteral("Monitor")),
              nullptr);
    EXPECT_TRUE(VNX::BuiltinLayouts::isReservedToken(QStringLiteral("Default")));
    EXPECT_FALSE(VNX::BuiltinLayouts::isReservedToken(QStringLiteral("Monitor")));
    EXPECT_FALSE(VNX::BuiltinLayouts::isReservedToken(QStringLiteral("My Layout")));
}

TEST(NamedLayoutStore, RejectsCreatingReservedNames)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("Default"));
    EXPECT_FALSE(VNX::NamedLayoutStore::write(QStringLiteral("Default"), payload));
    EXPECT_FALSE(VNX::NamedLayoutStore::write(QStringLiteral("default_layout"), payload));
    EXPECT_TRUE(VNX::NamedLayoutStore::list().isEmpty());
}

TEST(NamedLayoutStore, MonitorNamesAreNotReserved)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject monitor;
    monitor.insert(QStringLiteral("name"), QStringLiteral("Monitor"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("Monitor"), monitor));

    QJsonObject monitorToken;
    monitorToken.insert(QStringLiteral("name"), QStringLiteral("monitor_layout"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(
        QStringLiteral("monitor_layout"), monitorToken));

    EXPECT_EQ(VNX::NamedLayoutStore::list().size(), 2);
}

TEST(NamedLayoutStore, ListOmitsBuiltinEntries)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("UserDesk"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("UserDesk"), payload));

    const auto layouts = VNX::NamedLayoutStore::list();
    ASSERT_EQ(layouts.size(), 1);
    EXPECT_EQ(layouts.front().name, QStringLiteral("UserDesk"));
}

TEST(LayoutController, LoadNamedLayoutFailsWhenFileMissing)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    VNX::LayoutController controller(nullptr);
    EXPECT_FALSE(controller.loadNamedLayout(QStringLiteral("MissingDesk")));
}

TEST(LayoutController, ResetNamedLayoutDoesNotCloseWorkspaceWhenFileMissing)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("MixA"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("MixA"), payload));

    VNX::LayoutController controller(nullptr);
    ASSERT_TRUE(controller.loadNamedLayout(QStringLiteral("MixA")));
    EXPECT_EQ(controller.currentNamedLayout(), QStringLiteral("MixA"));

    int closerCalls = 0;
    controller.setInstanceCloser([&closerCalls]() { ++closerCalls; });
    ASSERT_TRUE(VNX::NamedLayoutStore::remove(QStringLiteral("MixA")));

    EXPECT_FALSE(controller.resetCurrentLayout());
    EXPECT_EQ(closerCalls, 0);
    EXPECT_EQ(controller.currentNamedLayout(), QStringLiteral("MixA"));
}

TEST(NamedLayoutStore, ExistingCollisionCanBeRemovedAndRenamed)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QFile orphan(QDir(root.path()).filePath(QStringLiteral("default_layout.json")));
    ASSERT_TRUE(orphan.open(QIODevice::WriteOnly));
    orphan.write(R"({"name":"default_layout"})");
    orphan.close();

    QJsonObject existing;
    ASSERT_TRUE(VNX::NamedLayoutStore::read(QStringLiteral("default_layout"), &existing));
    EXPECT_EQ(existing.value(QStringLiteral("name")).toString(),
              QStringLiteral("default_layout"));

    EXPECT_TRUE(VNX::NamedLayoutStore::rename(QStringLiteral("default_layout"),
                                              QStringLiteral("RecoveredDesk")));
    EXPECT_FALSE(VNX::NamedLayoutStore::read(QStringLiteral("default_layout"), nullptr));
    EXPECT_TRUE(VNX::NamedLayoutStore::read(QStringLiteral("RecoveredDesk"), nullptr));
    EXPECT_TRUE(VNX::NamedLayoutStore::remove(QStringLiteral("RecoveredDesk")));
    EXPECT_TRUE(VNX::NamedLayoutStore::list().isEmpty());
}

TEST(NamedLayoutStore, OrdinaryNamesStillRoundTrip)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("MixA"));
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("MixA"), payload));
    EXPECT_TRUE(VNX::NamedLayoutStore::rename(QStringLiteral("MixA"),
                                              QStringLiteral("MixB")));
    EXPECT_TRUE(VNX::NamedLayoutStore::remove(QStringLiteral("MixB")));
    EXPECT_TRUE(VNX::NamedLayoutStore::list().isEmpty());
}

TEST(BuiltinLayouts, ResolveResetTargetKeepsUserLayoutCase)
{
    using VNX::BuiltinLayouts::ResetTargetKind;
    using VNX::BuiltinLayouts::resolveResetTarget;

    const auto mixA = resolveResetTarget(QStringLiteral("MixA"), QStringLiteral("MixA"));
    EXPECT_EQ(mixA.kind, ResetTargetKind::UserCurrent);
    EXPECT_EQ(mixA.canonical, QStringLiteral("MixA"));

    const auto lowered = resolveResetTarget(QStringLiteral("mixa"), QStringLiteral("MixA"));
    EXPECT_EQ(lowered.kind, ResetTargetKind::Invalid);
    EXPECT_EQ(lowered.canonical, QStringLiteral("mixa"));

    const auto emptyUsesCurrent = resolveResetTarget(QString(), QStringLiteral("MixA"));
    EXPECT_EQ(emptyUsesCurrent.kind, ResetTargetKind::UserCurrent);
    EXPECT_EQ(emptyUsesCurrent.canonical, QStringLiteral("MixA"));
}

TEST(BuiltinLayouts, ResolveResetTargetNormalizesBuiltinTokens)
{
    using VNX::BuiltinLayouts::ResetTargetKind;
    using VNX::BuiltinLayouts::resolveResetTarget;

    const auto fromId = resolveResetTarget(QStringLiteral("DEFAULT_LAYOUT"),
                                           QStringLiteral("MixA"));
    EXPECT_EQ(fromId.kind, ResetTargetKind::Builtin);
    EXPECT_EQ(fromId.canonical, QStringLiteral("default_layout"));

    const auto fromName = resolveResetTarget(QStringLiteral("Default"),
                                             QStringLiteral("MixA"));
    EXPECT_EQ(fromName.kind, ResetTargetKind::Builtin);
    EXPECT_EQ(fromName.canonical, QStringLiteral("default_layout"));

    const auto emptyOnDefault = resolveResetTarget(QString(),
                                                   QStringLiteral("default_layout"));
    EXPECT_EQ(emptyOnDefault.kind, ResetTargetKind::Builtin);
    EXPECT_EQ(emptyOnDefault.canonical, QStringLiteral("default_layout"));
}

TEST(BuiltinLayouts, DescribeLayoutStateMarksUserCurrent)
{
    const auto onDefault = VNX::BuiltinLayouts::describeLayoutState(
        QStringLiteral("default_layout"));
    ASSERT_EQ(onDefault.size(), 1);
    EXPECT_EQ(onDefault.front().name, QStringLiteral("default_layout"));
    EXPECT_TRUE(onDefault.front().isCurrent);

    const auto onUser = VNX::BuiltinLayouts::describeLayoutState(QStringLiteral("MixA"));
    ASSERT_EQ(onUser.size(), 2);
    EXPECT_EQ(onUser[0].name, QStringLiteral("default_layout"));
    EXPECT_FALSE(onUser[0].isCurrent);
    EXPECT_EQ(onUser[1].name, QStringLiteral("MixA"));
    EXPECT_TRUE(onUser[1].isCurrent);
    EXPECT_EQ(onUser[1].index, 1);
}

TEST(LayoutController, SelectBuiltinRejectsUnknownWithoutChangingCurrent)
{
    VNX::LayoutController controller(nullptr);
    EXPECT_FALSE(controller.selectBuiltinLayout(QStringLiteral("monitor_layout")));
    EXPECT_TRUE(controller.currentNamedLayout().isEmpty());
}

TEST(LayoutController, UnknownRestoreSpecsAreDropped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("DeskA"));
    payload.insert(QStringLiteral("instances"), QJsonArray{
        QJsonObject{
            {QStringLiteral("typeId"), QStringLiteral("MonitorUnknown")},
            {QStringLiteral("instanceId"), 1},
        },
    });
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("DeskA"), payload));

    QStringList dropped;
    VNX::LayoutController controller(nullptr);
    controller.setInstanceRestoreFactory(
        [&dropped](const VNX::DockInstanceSpec& spec) {
            dropped.append(spec.typeId);
            return false;
        });

    EXPECT_TRUE(controller.loadNamedLayout(QStringLiteral("DeskA")));
    ASSERT_EQ(dropped.size(), 1);
    EXPECT_EQ(dropped.front(), QStringLiteral("MonitorUnknown"));
    EXPECT_EQ(controller.currentNamedLayout(), QStringLiteral("DeskA"));
}

TEST(LayoutController, UnknownMonitorPackTypeIdsAreDropped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    NamedLayoutStoreOverride guard(root.path());

    const QStringList typeIds{
        QStringLiteral("MonitorPlayables"),
        QStringLiteral("MonitorEffects"),
        QStringLiteral("MonitorBuses"),
        QStringLiteral("MonitorDspTimeline"),
        QStringLiteral("MonitorLineage"),
        QStringLiteral("MonitorTapSelect"),
        QStringLiteral("MonitorWaveform"),
        QStringLiteral("MonitorScope"),
        QStringLiteral("MonitorSpectrum"),
        QStringLiteral("MonitorMeter"),
    };

    QJsonArray instances;
    for (const QString& typeId : typeIds) {
        instances.append(QJsonObject{
            {QStringLiteral("typeId"), typeId},
            {QStringLiteral("instanceId"), 1},
        });
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), QStringLiteral("DeskP4"));
    payload.insert(QStringLiteral("instances"), instances);
    ASSERT_TRUE(VNX::NamedLayoutStore::write(QStringLiteral("DeskP4"), payload));

    QStringList dropped;
    VNX::LayoutController controller(nullptr);
    controller.setInstanceRestoreFactory(
        [&dropped](const VNX::DockInstanceSpec& spec) {
            dropped.append(QStringLiteral("%1_%2").arg(spec.typeId).arg(spec.instanceId));
            return false;
        });

    EXPECT_TRUE(controller.loadNamedLayout(QStringLiteral("DeskP4")));
    ASSERT_EQ(dropped.size(), typeIds.size());
    for (int i = 0; i < typeIds.size(); ++i)
        EXPECT_EQ(dropped.at(i), typeIds.at(i) + QStringLiteral("_1"));
    EXPECT_EQ(controller.currentNamedLayout(), QStringLiteral("DeskP4"));
}
