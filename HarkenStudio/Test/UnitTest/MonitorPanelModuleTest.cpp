#include "Core/WorkspaceWindowTypes.h"

#include <gtest/gtest.h>

#include <QSet>

using namespace harken::studio;

TEST(MonitorPanelModule, WorkspacePanelKeysAreUnique)
{
    QSet<QString> typeIds;
    QSet<QString> docks;
    const auto descriptors = defaultWorkspaceWindowDescriptors();
    for (const auto& descriptor : descriptors) {
        typeIds.insert(descriptor.typeId);
        docks.insert(descriptor.dockObjectName);
    }
    EXPECT_EQ(typeIds.size(), descriptors.size());
    EXPECT_EQ(docks.size(), descriptors.size());
}
