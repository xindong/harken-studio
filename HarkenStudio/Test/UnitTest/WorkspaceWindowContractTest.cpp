#include "WorkspaceWindowTypes.h"
#include "Widgets/Common/StatusTip.h"

#include <gtest/gtest.h>

#include <QMainWindow>
#include <QSet>
#include <QStatusBar>

using namespace VNX;

TEST(EditorWorkspaceWindowContract, DescriptorsHaveUniqueIds)
{
    const WorkspaceWindowCatalog catalog = defaultWorkspaceWindowCatalog();
    QSet<QString> typeIds;
    QSet<QString> dockObjectNames;

    for (const WorkspaceWindowDescriptor& descriptor : catalog.descriptors()) {
        EXPECT_FALSE(descriptor.typeId.isEmpty());
        EXPECT_FALSE(descriptor.dockObjectName.isEmpty());
        EXPECT_FALSE(typeIds.contains(descriptor.typeId));
        EXPECT_FALSE(dockObjectNames.contains(descriptor.dockObjectName));
        typeIds.insert(descriptor.typeId);
        dockObjectNames.insert(descriptor.dockObjectName);
    }
}

TEST(EditorWorkspaceWindowContract, FloatingPanelStatusStaysWithItsOwningWindow)
{
    QMainWindow firstWindow;
    QMainWindow secondWindow;
    auto* firstStatus = firstWindow.statusBar();
    auto* secondStatus = secondWindow.statusBar();
    QWidget firstFloating(&firstWindow, Qt::Window);
    QWidget firstPanel(&firstFloating);
    QWidget secondFloating(&secondWindow, Qt::Window);
    QWidget secondPanel(&secondFloating);

    ASSERT_TRUE(firstFloating.isWindow());
    ASSERT_TRUE(secondFloating.isWindow());
    showStatusTip(&firstPanel, QStringLiteral("First panel feedback"));
    EXPECT_EQ(firstStatus->currentMessage(), QStringLiteral("First panel feedback"));
    EXPECT_TRUE(secondStatus->currentMessage().isEmpty());

    showStatusTip(&secondPanel, QStringLiteral("Second panel feedback"));
    EXPECT_EQ(firstStatus->currentMessage(), QStringLiteral("First panel feedback"));
    EXPECT_EQ(secondStatus->currentMessage(), QStringLiteral("Second panel feedback"));
}
