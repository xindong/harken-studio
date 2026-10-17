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

TEST(LanguageContextService, LegacyPersistedSfxIsReplacedByFirstProjectLanguage)
{
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LanguageContextLegacySetting");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    storage->beginGroup(QStringLiteral("LanguageContext"));
    storage->setValue(QStringLiteral("language"), QStringLiteral("SFX"));
    storage->endGroup();
    VNX::MemorySettingsStorage* raw = storage.get();
    VNX::LanguageContextService service(std::move(storage));
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    EXPECT_EQ(service.availableLanguages(), names({"Japanese", "English"}));
    raw->beginGroup(QStringLiteral("LanguageContext"));
    EXPECT_EQ(raw->value(QStringLiteral("language")).toString(), QStringLiteral("Japanese"));
    raw->endGroup();

    pm.closeProject();
}

TEST(LanguageContextService, PersistenceRoundTripThroughInjectedStorage)
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
    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));

    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    VNX::MemorySettingsStorage* raw = storage.get();
    {
        VNX::LanguageContextService first(std::move(storage));
        EXPECT_EQ(first.language(), QStringLiteral("Japanese"));
        first.setLanguage(QStringLiteral("English"));
        raw->beginGroup(QStringLiteral("LanguageContext"));
        EXPECT_EQ(raw->value(QStringLiteral("language")).toString(), QStringLiteral("English"));
        raw->endGroup();
    }
    auto restored = std::make_unique<VNX::MemorySettingsStorage>();
    restored->beginGroup(QStringLiteral("LanguageContext"));
    restored->setValue(QStringLiteral("language"), QStringLiteral("English"));
    restored->endGroup();
    VNX::LanguageContextService second(std::move(restored));
    EXPECT_EQ(second.language(), QStringLiteral("English"));

    pm.closeProject();
}

}
