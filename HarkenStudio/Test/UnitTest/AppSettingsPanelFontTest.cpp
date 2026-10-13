#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"

#include <gtest/gtest.h>

#include <memory>

namespace VNX {

class AppSettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& settings = AppSettings::instance();
        savedHierarchy_ = settings.panelFontSize(PanelId::Hierarchy);
        savedInformation_ = settings.panelFontSize(PanelId::Information);
    }

    void TearDown() override
    {
        restoreDefaultStorage();
        AppSettings::instance().setPanelFontSize(PanelId::Hierarchy, savedHierarchy_);
        AppSettings::instance().setPanelFontSize(PanelId::Information, savedInformation_);
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

    int savedHierarchy_ = 0;
    int savedInformation_ = 0;
};

TEST_F(AppSettingsTest, AppearanceRoundTripKeepsStringKeysAndIgnoresMeterOrphan)
{
    auto storage = std::make_unique<MemorySettingsStorage>();
    storage->beginGroup(QStringLiteral("Appearance"));
    storage->setValue(QStringLiteral("PanelFontSize_hierarchy"), 18);
    storage->setValue(QStringLiteral("PanelFontSize_information"), 9);
    storage->setValue(QStringLiteral("PanelFontSize_mixer"), 14);
    storage->setValue(QStringLiteral("PanelFontSize_meter"), 21);
    storage->endGroup();

    MemorySettingsStorage* raw = installMemoryStorage(std::move(storage));

    auto& settings = AppSettings::instance();
    EXPECT_EQ(settings.panelFontSize(PanelId::Hierarchy), 18);
    EXPECT_EQ(settings.panelFontSize(PanelId::Information), 9);
    EXPECT_EQ(settings.panelFontSize(QStringLiteral("hierarchy")), 18);
    EXPECT_EQ(settings.panelFontSize(QStringLiteral("mixer")), AppSettings::DEFAULT_FONT_SIZE);
    EXPECT_EQ(settings.panelFontSize(QStringLiteral("meter")), AppSettings::DEFAULT_FONT_SIZE);

    settings.setPanelFontSize(QStringLiteral("meter"), 8);
    EXPECT_EQ(settings.panelFontSize(PanelId::Hierarchy), 18);
    EXPECT_EQ(settings.panelFontSize(PanelId::Information), 9);
    EXPECT_EQ(settings.panelFontSize(QStringLiteral("mixer")), AppSettings::DEFAULT_FONT_SIZE);
    EXPECT_EQ(settings.panelFontSize(QStringLiteral("meter")), AppSettings::DEFAULT_FONT_SIZE);

    settings.save();

    raw->beginGroup(QStringLiteral("Appearance"));
    EXPECT_EQ(raw->value(QStringLiteral("PanelFontSize_hierarchy")).toInt(), 18);
    EXPECT_EQ(raw->value(QStringLiteral("PanelFontSize_information")).toInt(), 9);
    EXPECT_EQ(raw->value(QStringLiteral("PanelFontSize_mixer")).toInt(), 14);
    EXPECT_EQ(raw->value(QStringLiteral("PanelFontSize_meter")).toInt(), 21);
    raw->endGroup();
}

} // namespace VNX
