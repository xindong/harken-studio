#include "Widgets/Monitor/MonitorPanelKeys.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QStringList>

using namespace VNX;

namespace {

QSet<QString> toSet(const QStringList& values)
{
    return QSet<QString>(values.begin(), values.end());
}

}

TEST(MonitorPanelModule, PanelKeysAreUnique)
{
    const QStringList typeIds = monitorPanelTypeIds();
    const QStringList docks = monitorPanelDockObjectNames();
    EXPECT_EQ(toSet(typeIds).size(), typeIds.size());
    EXPECT_EQ(toSet(docks).size(), docks.size());
}
