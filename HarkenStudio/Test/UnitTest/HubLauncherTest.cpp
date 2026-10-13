#include "Core/HubLauncher.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

TEST(HubLauncherTest, PrefersConfiguredInstallDirectory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
#ifdef Q_OS_MACOS
    const QString executable = QDir(directory.path()).filePath(
        QStringLiteral("HarkenHub.app/Contents/MacOS/HarkenHub"));
#else
    const QString executable =
        QDir(directory.path()).filePath(QStringLiteral("HarkenHub.exe"));
#endif
    ASSERT_TRUE(QDir().mkpath(QFileInfo(executable).absolutePath()));
    QFile file(executable);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    const QByteArray previous = qgetenv("HARKEN_HUB_INSTALL_DIR");
    qputenv("HARKEN_HUB_INSTALL_DIR", directory.path().toUtf8());
    EXPECT_EQ(VNX::HubLauncher::executablePath(),
              QFileInfo(executable).absoluteFilePath());
    if (previous.isNull())
        qunsetenv("HARKEN_HUB_INSTALL_DIR");
    else
        qputenv("HARKEN_HUB_INSTALL_DIR", previous);
}

TEST(HubLauncherTest, LocalBuildPrefersTheColocatedHub)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
#ifdef Q_OS_MACOS
    const QString expected = QDir(directory.path()).filePath(
        QStringLiteral("HarkenHub.app/Contents/MacOS/HarkenHub"));
#else
    const QString expected =
        QDir(directory.path()).filePath(QStringLiteral("HarkenHub.exe"));
#endif
    ASSERT_TRUE(QDir().mkpath(QFileInfo(expected).absolutePath()));
    QFile file(expected);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    const QByteArray previous = qgetenv("HARKEN_HUB_INSTALL_DIR");
    qunsetenv("HARKEN_HUB_INSTALL_DIR");
    EXPECT_EQ(VNX::HubLauncher::resolveExecutablePath(true, directory.path()),
              QFileInfo(expected).absoluteFilePath());
    if (!previous.isNull())
        qputenv("HARKEN_HUB_INSTALL_DIR", previous);
}

TEST(HubLauncherTest, PassesTheLaunchingStudioExecutableToHub)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString studioExecutable =
        directory.filePath(QStringLiteral("HarkenStudio.exe"));

    const QStringList arguments = VNX::HubLauncher::launchArguments(
        VNX::HubPage::Projects, studioExecutable);

    EXPECT_EQ(arguments,
              QStringList({QStringLiteral("--show-projects"),
                           QStringLiteral("--register-local-build=%1")
                               .arg(QFileInfo(studioExecutable)
                                        .absoluteFilePath())}));
}

TEST(HubLauncherTest, DirectLaunchRequestsTheVersionReveal)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QStringList arguments = VNX::HubLauncher::launchArguments(
        VNX::HubPage::Projects,
        directory.filePath(QStringLiteral("HarkenStudio.exe")), true);

    EXPECT_TRUE(arguments.at(1).startsWith(
        QStringLiteral("--register-local-build=")));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--reveal-local-build")));
}

TEST(HubLauncherTest, OpensIntegrationEditorWithCurrentProject)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString studioExecutable =
        directory.filePath(QStringLiteral("HarkenStudio.exe"));
    const QString projectPath =
        directory.filePath(QStringLiteral("Game.hsp"));
    const QStringList arguments = VNX::HubLauncher::launchArguments(
        VNX::HubPage::EditIntegration, studioExecutable, false, projectPath);

    EXPECT_EQ(arguments.first(), QStringLiteral("--edit-integration"));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--register-local-build=%1")
            .arg(QFileInfo(studioExecutable).absoluteFilePath())));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--harken-project=%1")
            .arg(QFileInfo(projectPath).absoluteFilePath())));
    EXPECT_EQ(arguments.size(), 3);
}
