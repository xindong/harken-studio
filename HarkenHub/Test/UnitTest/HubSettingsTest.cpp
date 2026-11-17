#include "Settings/HubSettings.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <memory>
#include <vector>

using VNH::HubSettings;

namespace {

class HubSettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
    }

    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    QTemporaryDir dir;
};

TEST_F(HubSettingsTest, PersistsAutomaticSilentHubUpdateChoice)
{
    ASSERT_TRUE(HubSettings::setAutoSilentlyUpdateHub(false));
    EXPECT_FALSE(HubSettings::autoSilentlyUpdateHub());

    ASSERT_TRUE(HubSettings::setAutoSilentlyUpdateHub(true));
    EXPECT_TRUE(HubSettings::autoSilentlyUpdateHub());
}

TEST_F(HubSettingsTest, PersistsAutomaticStudioDownloadChoice)
{
    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(false));
    EXPECT_FALSE(HubSettings::autoDownloadStudio());

    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(true));
    EXPECT_TRUE(HubSettings::autoDownloadStudio());
}

TEST_F(HubSettingsTest, PersistsProjectLocation)
{
    const QString path = dir.filePath(QStringLiteral("Harken Projects"));
    ASSERT_TRUE(HubSettings::setProjectLocation(path));
    EXPECT_EQ(HubSettings::projectLocation(), path);
}

TEST_F(HubSettingsTest, PersistsLanguagePreference)
{
    EXPECT_EQ(HubSettings::languagePreference(), QStringLiteral("system"));
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("zh_CN")));
    EXPECT_EQ(HubSettings::languagePreference(), QStringLiteral("zh_CN"));
}

TEST_F(HubSettingsTest, PersistsWindowSize)
{
    EXPECT_FALSE(HubSettings::windowSize().isValid());
    ASSERT_TRUE(HubSettings::setWindowSize(QSize(1440, 900)));
    EXPECT_EQ(HubSettings::windowSize(), QSize(1440, 900));

    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject windowSize =
        saved.value(QStringLiteral("windowSize")).toObject();
    EXPECT_EQ(windowSize.value(QStringLiteral("width")).toInt(), 1440);
    EXPECT_EQ(windowSize.value(QStringLiteral("height")).toInt(), 900);
}

TEST_F(HubSettingsTest, PersistsWindowPositionIncludingOrigin)
{
    EXPECT_FALSE(HubSettings::windowPosition().has_value());
    ASSERT_TRUE(HubSettings::setWindowPosition(QPoint(80, 80)));
    EXPECT_EQ(HubSettings::windowPosition(), QPoint(80, 80));
    ASSERT_TRUE(HubSettings::setWindowPosition(QPoint(0, 0)));
    EXPECT_EQ(HubSettings::windowPosition(), QPoint(0, 0));
}

TEST_F(HubSettingsTest, IgnoresCorruptWindowPosition)
{
    ASSERT_TRUE(HubSettings::setWindowPosition(QPoint(80, 80)));
    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject corrupt;
    corrupt.insert(QStringLiteral("x"), QJsonValue::Null);
    corrupt.insert(QStringLiteral("y"), QStringLiteral("bad"));
    root.insert(QStringLiteral("windowPosition"), corrupt);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    EXPECT_FALSE(HubSettings::windowPosition().has_value());
}

TEST_F(HubSettingsTest, PersistsAndConditionallyClearsPendingUpdateNotesVersion)
{
    EXPECT_TRUE(HubSettings::pendingUpdateNotesVersion().isEmpty());
    ASSERT_TRUE(HubSettings::setPendingUpdateNotesVersion(
        QStringLiteral("0.1.39-dev")));
    EXPECT_EQ(HubSettings::pendingUpdateNotesVersion(),
              QStringLiteral("0.1.39-dev"));
    EXPECT_FALSE(HubSettings::clearPendingUpdateNotesVersion(
        QStringLiteral("0.1.38-dev")));
    EXPECT_EQ(HubSettings::pendingUpdateNotesVersion(),
              QStringLiteral("0.1.39-dev"));
    ASSERT_TRUE(HubSettings::clearPendingUpdateNotesVersion(
        QStringLiteral("0.1.39-dev")));
    EXPECT_TRUE(HubSettings::pendingUpdateNotesVersion().isEmpty());
}

TEST_F(HubSettingsTest, InvalidWindowSizeDoesNotReplaceSavedValue)
{
    ASSERT_TRUE(HubSettings::setWindowSize(QSize(1280, 720)));
    EXPECT_FALSE(HubSettings::setWindowSize(QSize()));
    EXPECT_EQ(HubSettings::windowSize(), QSize(1280, 720));
}

TEST_F(HubSettingsTest, UnsupportedLanguagePreferenceIsRejected)
{
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("en")));
    EXPECT_FALSE(HubSettings::setLanguagePreference(QString()));
    EXPECT_FALSE(
        HubSettings::setLanguagePreference(QStringLiteral("unknown")));
    EXPECT_EQ(HubSettings::languagePreference(), QStringLiteral("en"));
}

TEST_F(HubSettingsTest, EmptyProjectLocationDoesNotReplaceSavedValue)
{
    const QString path = dir.filePath(QStringLiteral("Harken Projects"));
    ASSERT_TRUE(HubSettings::setProjectLocation(path));
    EXPECT_FALSE(HubSettings::setProjectLocation(QStringLiteral("   ")));
    EXPECT_EQ(HubSettings::projectLocation(), path);
}

TEST_F(HubSettingsTest, PreservesOtherSettings)
{
    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    QJsonObject initial;
    initial[QStringLiteral("language")] = QStringLiteral("English");
    file.write(QJsonDocument(initial).toJson());
    file.close();

    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(false));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_EQ(saved.value(QStringLiteral("language")).toString(),
              QStringLiteral("English"));
    EXPECT_FALSE(saved.value(QStringLiteral("autoDownloadStudio")).toBool());
}

TEST_F(HubSettingsTest, AutomaticHubUpdatePreservesOtherSettings)
{
    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(false));
    ASSERT_TRUE(HubSettings::setAutoSilentlyUpdateHub(false));

    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_FALSE(saved.value(QStringLiteral("autoDownloadStudio")).toBool());
    EXPECT_FALSE(
        saved.value(QStringLiteral("autoSilentlyUpdateHub")).toBool());
}

TEST_F(HubSettingsTest, ProjectLocationPreservesOtherSettings)
{
    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(false));
    ASSERT_TRUE(HubSettings::setProjectLocation(
        QStringLiteral("D:/Harken Projects")));

    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_FALSE(saved.value(QStringLiteral("autoDownloadStudio")).toBool());
    EXPECT_EQ(saved.value(QStringLiteral("projectLocation")).toString(),
              QStringLiteral("D:/Harken Projects"));
}

TEST_F(HubSettingsTest, ConcurrentProcessesPreserveIndependentPreferencesAndWindowState)
{
    ASSERT_TRUE(HubSettings::setAutoDownloadStudio(false));
    std::vector<std::unique_ptr<QProcess>> clients;
    for (int i = 0; i < 12; ++i) {
        auto client = std::make_unique<QProcess>();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "invalid-platform-cli-must-not-load");
        client->setProcessEnvironment(environment);
        client->start(QString::fromUtf8(HARKEN_HUB_CLI_FIXTURE),
            {"--cli", "settings", "set", "--key", i % 2 ? "theme" : "language", "--value", i % 2 ? "dark" : "zh_CN"});
        ASSERT_TRUE(client->waitForStarted());
        clients.push_back(std::move(client));
    }
    for (int i = 1; i <= 50; ++i) {
        ASSERT_TRUE(HubSettings::setWindowPosition(QPoint(i, i)));
        ASSERT_TRUE(HubSettings::setTableSort(QString::number(i), {i, Qt::DescendingOrder}));
    }
    for (const auto& client : clients) {
        ASSERT_TRUE(client->waitForFinished(15000));
        EXPECT_EQ(client->exitCode(), 0) << client->readAllStandardOutput().toStdString();
    }
    EXPECT_EQ(HubSettings::themePreference(), "dark");
    EXPECT_EQ(HubSettings::languagePreference(), "zh_CN");
    EXPECT_FALSE(HubSettings::autoDownloadStudio());
    EXPECT_EQ(HubSettings::windowPosition(), QPoint(50, 50));
    for (int i = 1; i <= 50; ++i) EXPECT_EQ(HubSettings::tableSort(QString::number(i)).column, i);
}

TEST_F(HubSettingsTest, InvalidFileFallsBackToDefault)
{
    QFile file(HubSettings::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("invalid");
    file.close();

    EXPECT_TRUE(HubSettings::autoDownloadStudio());
    EXPECT_TRUE(HubSettings::autoSilentlyUpdateHub());
}

}
