#include "Launch/ProjectCreateFlow.h"
#include "Platform/ProcessScan.h"
#include "Versions/LocalBuildRegistry.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

TEST(ProjectCreateFlowTest, DispatchDoesNotWaitForProjectCreation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNH::ProjectCreateRequest request;
    request.version.installPath = QFileInfo(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE)).absolutePath();
    request.version.executablePath = QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE);
    request.version.capabilitiesVersion = 2;
    request.name = QStringLiteral("Demo");
    request.location = directory.path();
    QFile scenario(directory.filePath("Demo.scenario.json"));
    ASSERT_TRUE(scenario.open(QIODevice::WriteOnly));
    scenario.write("{}");
    scenario.close();
    QString error;
    ASSERT_TRUE(VNH::ProjectCreateFlow::create(request, &error)) << error.toStdString();
    const auto path = directory.filePath("Demo/Demo.hsp");
    QFile log(path + ".launch.json");
    QElapsedTimer timer;
    timer.start();
    while (!log.exists() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    ASSERT_TRUE(log.open(QIODevice::ReadOnly));
    const auto pid = QJsonDocument::fromJson(log.readAll()).object().value("pid").toInteger();
    EXPECT_GT(pid, 0);
    EXPECT_FALSE(QFileInfo::exists(path));
    EXPECT_FALSE(VNH::executablePathForPid(pid).isEmpty());
    ASSERT_TRUE(VNH::terminateProcessById(pid));
    while (!VNH::executablePathForPid(pid).isEmpty() && timer.elapsed() < 15000)
        QThread::msleep(5);
}

TEST(ProjectCreateFlowTest, MissingExecutableReturnsTheDispatchError)
{
    QTemporaryDir directory;
    VNH::ProjectCreateRequest request;
    request.version.installPath = directory.path();
    request.version.executablePath = directory.filePath("MissingStudio.exe");
    request.version.capabilitiesVersion = 2;
    QString error;
    EXPECT_FALSE(VNH::ProjectCreateFlow::create(request, &error));
    EXPECT_TRUE(error.contains(request.version.executablePath));
}
