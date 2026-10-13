#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"

#include <gtest/gtest.h>

#include <memory>

#include <QObject>
#include <QString>

namespace VNX {

class AppSettingsLanguageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        savedLanguage_ = AppSettings::instance().language();
    }

    void TearDown() override
    {
        restoreDefaultStorage();
        AppSettings::instance().setLanguage(savedLanguage_);
    }

    MemorySettingsStorage* installMemoryStorage(std::unique_ptr<MemorySettingsStorage> storage)
    {
        MemorySettingsStorage* raw = storage.get();
        AppSettings::instance().setStorage(std::move(storage));
        return raw;
    }

    void restoreDefaultStorage()
    {
        AppSettings::instance().setStorage(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
    }

    QString savedLanguage_;
};

TEST_F(AppSettingsLanguageTest, EmptyStorageDefaultsToEnglish)
{
    installMemoryStorage(std::make_unique<MemorySettingsStorage>());
    EXPECT_EQ(AppSettings::instance().language(), QStringLiteral("en"));
}

TEST_F(AppSettingsLanguageTest, SetLanguageEmitsAndPersists)
{
    auto storage = std::make_unique<MemorySettingsStorage>();
    MemorySettingsStorage* raw = installMemoryStorage(std::move(storage));

    int emitted = 0;
    QString lastCode;
    QMetaObject::Connection connection = QObject::connect(
        &AppSettings::instance(), &AppSettings::languageChanged,
        [&](const QString& code) {
            ++emitted;
            lastCode = code;
        });
    AppSettings::instance().setLanguage(QStringLiteral("zh-CN"));
    QObject::disconnect(connection);

    EXPECT_EQ(AppSettings::instance().language(), QStringLiteral("zh-CN"));
    EXPECT_EQ(emitted, 1);
    EXPECT_EQ(lastCode, QStringLiteral("zh-CN"));

    raw->beginGroup(QStringLiteral("Appearance"));
    EXPECT_EQ(raw->value(QStringLiteral("Language")).toString(), QStringLiteral("zh-CN"));
    raw->endGroup();
}

TEST_F(AppSettingsLanguageTest, LoadReadsPersistedLanguage)
{
    auto storage = std::make_unique<MemorySettingsStorage>();
    storage->beginGroup(QStringLiteral("Appearance"));
    storage->setValue(QStringLiteral("Language"), QStringLiteral("zh-CN"));
    storage->endGroup();

    installMemoryStorage(std::move(storage));
    EXPECT_EQ(AppSettings::instance().language(), QStringLiteral("zh-CN"));
}

} // namespace VNX
