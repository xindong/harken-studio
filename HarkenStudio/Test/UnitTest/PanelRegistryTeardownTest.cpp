#include "PanelInstanceRegistry.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QPointer>
#include <QWidget>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

// A full MainWindow cannot be instantiated in this harness (it drags in the
// audio engine, IPC server, and project singletons), so these tests reproduce
// the exact ownership seam of the shutdown crash instead: an owner whose
// PanelInstanceRegistry child is created before the dock subtree, meaning the
// registry is destroyed first during owner teardown. Dock-destruction cleanup
// must therefore be tied to the registry's lifetime (connection context), not
// to the longer-lived owner.
struct TeardownFixture
{
    QWidget* owner = nullptr;
    PanelInstanceRegistry* registry = nullptr;
    QWidget* dock = nullptr;
    QPointer<PanelInstanceRegistry> guard;

    TeardownFixture()
    {
        ensureApp();
        owner = new QWidget;
        registry = new PanelInstanceRegistry(owner);
        dock = new QWidget(owner);
        guard = registry;
        auto* panel = new QWidget(dock);
        registry->registerPanel(QStringLiteral("Meter"), 1, panel);
    }
};

} // namespace

TEST(PanelRegistryTeardown, RegistryDiesBeforeDockDuringOwnerTeardown)
{
    TeardownFixture f;
    bool registryAliveAtDockDeath = true;
    QObject::connect(f.dock, &QObject::destroyed, f.dock,
        [&registryAliveAtDockDeath, guard = f.guard]() {
            registryAliveAtDockDeath = !guard.isNull();
        });

    delete f.owner;

    EXPECT_FALSE(registryAliveAtDockDeath);
    EXPECT_TRUE(f.guard.isNull());
}

TEST(PanelRegistryTeardown, RegistryContextCleanupIsSkippedWhenRegistryDiesFirst)
{
    TeardownFixture f;
    bool cleanupRan = false;
    QObject::connect(f.dock, &QObject::destroyed, f.registry,
        [&cleanupRan, guard = f.guard]() {
            cleanupRan = true;
            if (guard)
                guard->unregisterPanel(QStringLiteral("Meter"), 1);
        });

    delete f.owner;

    EXPECT_FALSE(cleanupRan);
    EXPECT_TRUE(f.guard.isNull());
}

TEST(PanelRegistryTeardown, OwnerContextCleanupStillFiresAfterRegistryDeath)
{
    // The defective pre-fix shape: with the owner as connection context, Qt
    // still invokes the cleanup while the owner is partially torn down and
    // the registry is already destroyed. A raw registry pointer here is a
    // use-after-free; the QPointer guard observes the dead registry safely.
    TeardownFixture f;
    bool cleanupRan = false;
    bool registryAliveAtCleanup = true;
    QObject::connect(f.dock, &QObject::destroyed, f.owner,
        [&cleanupRan, &registryAliveAtCleanup, guard = f.guard]() {
            cleanupRan = true;
            registryAliveAtCleanup = !guard.isNull();
        });

    delete f.owner;

    EXPECT_TRUE(cleanupRan);
    EXPECT_FALSE(registryAliveAtCleanup);
}

TEST(PanelRegistryTeardown, RegistryContextCleanupRunsOnRuntimeDockClose)
{
    TeardownFixture f;
    bool cleanupRan = false;
    QObject::connect(f.dock, &QObject::destroyed, f.registry,
        [&cleanupRan, guard = f.guard]() {
            cleanupRan = true;
            if (guard)
                guard->unregisterPanel(QStringLiteral("Meter"), 1);
        });

    ASSERT_EQ(f.registry->allPanels(QStringLiteral("Meter")).size(), 1);
    delete f.dock;

    EXPECT_TRUE(cleanupRan);
    EXPECT_TRUE(f.registry->allPanels(QStringLiteral("Meter")).isEmpty());

    delete f.owner;
}
