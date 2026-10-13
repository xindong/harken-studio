#include "Core/IPC/IPCServer.h"
#include "Core/ThemeManager.h"

#include <QJsonObject>

#include <gtest/gtest.h>

TEST(ThemeIpc, ReportsCurrentStableThemeId)
{
    VNX::IPCServer server(nullptr);

    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("get_theme"), QJsonObject{});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("theme")).toString(),
              VNX::ThemeManager::instance().currentThemeId());
}

TEST(ThemeIpc, DarkAliasSelectsBuiltinTheme)
{
    VNX::IPCServer server(nullptr);

    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("set_theme"),
        QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("theme")).toString(),
              VNX::builtinThemeId());
}

TEST(ThemeIpc, RejectsMissingThemeWithoutChangingSelection)
{
    VNX::IPCServer server(nullptr);
    const QString before = VNX::ThemeManager::instance().currentThemeId();

    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("set_theme"),
        QJsonObject{{QStringLiteral("theme"),
                     QStringLiteral("theme.does-not-exist")}});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(VNX::ThemeManager::instance().currentThemeId(), before);
}
