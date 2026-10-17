#include "Core/LanguageContextService.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

QStringList names(std::initializer_list<const char*> raw)
{
    QStringList out;
    for (const char* name : raw)
        out.append(QString::fromLatin1(name));
    return out;
}

TEST(LanguageContextService, DefaultsToEnglishAndRejectsUnknownLanguages)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextDefault");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));

    VNX::LanguageContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    EXPECT_EQ(service.availableLanguages(), names({"English"}));
    EXPECT_EQ(service.language(), QStringLiteral("English"));

    service.setLanguage(QStringLiteral("English"));
    EXPECT_EQ(service.language(), QStringLiteral("English"));

    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));
    EXPECT_EQ(service.availableLanguages(), names({"Japanese", "English"}));
    EXPECT_EQ(service.language(), QStringLiteral("English"));
    service.setLanguage(QStringLiteral("Unknown"));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    service.setLanguage(QStringLiteral("english"));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    service.setLanguage(QStringLiteral("English"));
    EXPECT_EQ(service.language(), QStringLiteral("English"));

    pm.closeProject();
    EXPECT_TRUE(service.language().isEmpty());
}

TEST(LanguageContextService, ProjectLanguageChangesKeepOnlyExactIntersection)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextProjectChanges");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    pm.setProjectLanguagesInternal(names({"English", "Japanese"}));

    VNX::LanguageContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    service.setLanguage(QStringLiteral("Japanese"));

    pm.setProjectLanguagesInternal(names({"Korean", "Japanese", "English"}));
    EXPECT_EQ(service.availableLanguages(), names({"Korean", "Japanese", "English"}));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));

    pm.setProjectLanguagesInternal(names({"Korean", "japanese", "English"}));
    EXPECT_EQ(service.language(), QStringLiteral("Korean"));

    pm.setProjectLanguagesInternal({});
    EXPECT_EQ(service.availableLanguages(), names({"Korean", "japanese", "English"}));
    EXPECT_EQ(service.language(), QStringLiteral("Korean"));

    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));

    pm.closeProject();
}

TEST(LanguageContextService, ProjectLanguageCommandsNormalizeAndRefreshContext)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextCommands");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    VNX::LanguageContextService service(std::make_unique<VNX::MemorySettingsStorage>());

    EXPECT_TRUE(pm.addProjectLanguage(QStringLiteral(" Japanese ")));
    EXPECT_EQ(service.availableLanguages(), names({"English", "Japanese"}));
    EXPECT_EQ(service.language(), QStringLiteral("English"));
    EXPECT_TRUE(pm.addProjectLanguage(QStringLiteral("Japanese")));
    EXPECT_FALSE(pm.addProjectLanguage(QStringLiteral("japanese")));
    EXPECT_FALSE(pm.addProjectLanguage(QStringLiteral("SFX")));
    EXPECT_FALSE(pm.addProjectLanguage(QStringLiteral(" ")));
    EXPECT_EQ(pm.getProjectLanguages(), names({"English", "Japanese"}));

    EXPECT_TRUE(pm.addProjectLanguage(QStringLiteral("English")));
    service.setLanguage(QStringLiteral("English"));
    EXPECT_TRUE(pm.removeProjectLanguage(QStringLiteral("English")));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    EXPECT_FALSE(pm.removeProjectLanguage(QStringLiteral("Japanese")));
    EXPECT_EQ(service.availableLanguages(), names({"Japanese"}));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    pm.closeProject();
}

TEST(LanguageContextService, InvalidSavedLanguageIsReplacedByFirstProjectLanguage)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextInvalidSetting");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    const QString group = QStringLiteral("LanguageContext/%1").arg(pm.currentProject()->projectId);
    storage->beginGroup(group);
    storage->setValue(QStringLiteral("language"), QStringLiteral("SFX"));
    storage->endGroup();
    VNX::MemorySettingsStorage* raw = storage.get();
    VNX::LanguageContextService service(std::move(storage));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    EXPECT_EQ(service.availableLanguages(), names({"Japanese", "English"}));
    raw->beginGroup(group);
    EXPECT_EQ(raw->value(QStringLiteral("language")).toString(), QStringLiteral("Japanese"));
    raw->endGroup();

    pm.closeProject();
}

TEST(LanguageContextService, ProjectSelectionsSurviveClosingSwitchingAndServiceRecreation)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextPersist");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    pm.setProjectLanguagesInternal(names({"English", "Japanese"}));
    ASSERT_TRUE(pm.saveProject());
    const QString firstPath = pm.currentProject()->projectPath;
    const QString firstId = pm.currentProject()->projectId;

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    storage->setValue(QStringLiteral("LanguageContext/language"), QStringLiteral("Japanese"));
    VNX::MemorySettingsStorage* raw = storage.get();
    auto restored = std::make_unique<VNX::MemorySettingsStorage>();
    QString secondPath;
    {
        VNX::LanguageContextService service(std::move(storage));
        EXPECT_EQ(service.language(), QStringLiteral("English"));
        service.setLanguage(QStringLiteral("Japanese"));
        ASSERT_TRUE(pm.closeProject());
        EXPECT_TRUE(service.language().isEmpty());
        EXPECT_TRUE(service.availableLanguages().isEmpty());
        service.setLanguage(QStringLiteral("English"));
        EXPECT_TRUE(service.language().isEmpty());

        config.location = tempDir.filePath(QStringLiteral("Second"));
        ASSERT_TRUE(pm.createProject(config));
        ASSERT_NE(pm.currentProject()->projectId, firstId);
        pm.setProjectLanguagesInternal(names({"English", "Japanese"}));
        ASSERT_TRUE(pm.saveProject());
        secondPath = pm.currentProject()->projectPath;
        EXPECT_EQ(service.language(), QStringLiteral("English"));

        ASSERT_TRUE(pm.loadProject(firstPath));
        EXPECT_EQ(pm.currentProject()->projectId, firstId);
        EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
        ASSERT_TRUE(pm.loadProject(secondPath));
        EXPECT_EQ(service.language(), QStringLiteral("English"));
        ASSERT_TRUE(pm.closeProject());
        EXPECT_TRUE(service.language().isEmpty());

        for (const auto& key : raw->allKeys())
            restored->setValue(key, raw->value(key));
    }
    VNX::LanguageContextService reopened(std::move(restored));
    EXPECT_TRUE(reopened.language().isEmpty());
    ASSERT_TRUE(pm.loadProject(firstPath));
    EXPECT_EQ(reopened.language(), QStringLiteral("Japanese"));
    ASSERT_TRUE(pm.removeProjectLanguage(QStringLiteral("Japanese")));
    EXPECT_EQ(reopened.language(), QStringLiteral("English"));
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.closeProject());
    EXPECT_TRUE(reopened.language().isEmpty());
    ASSERT_TRUE(pm.loadProject(firstPath));
    EXPECT_EQ(reopened.language(), QStringLiteral("English"));
    ASSERT_TRUE(pm.loadProject(secondPath));
    EXPECT_EQ(reopened.language(), QStringLiteral("English"));

    pm.closeProject();
}

}
