#include "Core/AI/AIAgentConfig.h"
#include "Core/AppSettings.h"
#include "Core/ColorPalette.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Dialogs/ColorEditorDialog.h"
#include "Dialogs/Settings/PreferencesPanel.h"
#include "Dialogs/Settings/Pages/AppearancePage.h"
#include "Widgets/Common/LinearSliderPresenter.h"
#include "Widgets/Common/ParameterBinding.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <gtest/gtest.h>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}


bool selectTheme(QComboBox& themes, const QString& id)
{
    const int index = themes.findData(id);
    if (index < 0) return false;
    themes.setCurrentIndex(index);
    return true;
}

bool applyColorPreset(QComboBox& presets, const QString& id)
{
    const int index = presets.findData(id);
    if (index < 0) return false;
    presets.setCurrentIndex(index);
    presets.activated(index);
    return true;
}

bool selectColor(QTreeWidget& colors, const QString& key)
{
    for (int i = 0; i < colors.topLevelItemCount(); ++i) {
        auto* item = colors.topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == key) {
            colors.setCurrentItem(item);
            return true;
        }
    }
    return false;
}

VNX::ColorEditorDialog* openColorEditor(QWidget& page, QTreeWidget& colors)
{
    if (!colors.currentItem()) return nullptr;
    colors.itemDoubleClicked(colors.currentItem(), 1);
    return page.findChild<VNX::ColorEditorDialog*>(QStringLiteral("appearanceColorDialog"));
}

bool finishColorEdit(QWidget& page, QTreeWidget& colors, const QColor& color)
{
    auto* picker = openColorEditor(page, colors);
    if (!picker) return false;
    picker->setCurrentColor(color);
    picker->accept();
    QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);
    return true;
}

QVariantMap storedValues(const VNX::ISettingsStorage& storage)
{
    QVariantMap values;
    for (const QString& key : storage.allKeys())
        values.insert(key, storage.value(key));
    return values;
}

class WritableSettingsStorage : public VNX::MemorySettingsStorage
{
public:
    bool isWritable() const override { return writable; }
    bool writable = true;
};

}

namespace VNX {

class SettingsApplyContractTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(root_.isValid());
        AppSettings::instance().setStorage(std::make_unique<MemorySettingsStorage>());
        AIAgentConfigStore::setStorageOverrideForTesting(std::make_shared<MemorySettingsStorage>());
        originalConfig_.baseUrl = QStringLiteral("https://original.example/v1");
        originalConfig_.model = QStringLiteral("original-model");
        AIAgentConfigStore().save(originalConfig_);
        auto storage = std::make_unique<MemorySettingsStorage>();
        themeStorage_ = storage.get();
        theme_ = std::make_unique<ThemeManager>(root_.path(), std::move(storage));
    }

    void TearDown() override
    {
        theme_.reset();
        AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
        AppSettings::instance().setStorage(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
    }

    PreferencesPanel::SecretStoreAccess secretStore()
    {
        return {
            [this](const QString&, const QString&) { return secret_; },
            [this](const QString&, const QString&, const QString& value) {
                if (failSecretWrite_) return false;
                secret_ = value;
                return true;
            },
            [this](const QString&, const QString&) {
                if (failSecretWrite_) return false;
                secret_.clear();
                return true;
            }
        };
    }

    QTemporaryDir root_;
    std::unique_ptr<ThemeManager> theme_;
    MemorySettingsStorage* themeStorage_ = nullptr;
    AIAgentConfig originalConfig_;
    QString secret_ = QStringLiteral("original-key");
    bool failSecretWrite_ = false;
};

TEST_F(SettingsApplyContractTest, FailedConnectionCommitKeepsIndependentPreferences)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* language = panel.findChild<QComboBox*>(QStringLiteral("appearanceLanguageCombo"));
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("aiConnectionSaveButton"));
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(language, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);

    developer->setChecked(true);
    language->setCurrentIndex(language->findData(QStringLiteral("zh-CN")));
    url->setText(QStringLiteral("https://replacement.example/v1"));
    key->setText(QStringLiteral("replacement-key"));
    failSecretWrite_ = true;
    ASSERT_TRUE(save->isEnabled());
    save->click();

    EXPECT_TRUE(AppSettings::instance().developerMode());
    EXPECT_EQ(AppSettings::instance().language(), QStringLiteral("zh-CN"));
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == originalConfig_);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_TRUE(panel.findChild<AIAgentPage*>()->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, OpenPreferencesFollowExternalCommittedValues)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* language = panel.findChild<QComboBox*>(QStringLiteral("appearanceLanguageCombo"));
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(language, nullptr);

    AppSettings::instance().setDeveloperMode(true);
    AppSettings::instance().setLanguage(QStringLiteral("zh-CN"));
    EXPECT_TRUE(developer->isChecked());
    EXPECT_EQ(language->currentData().toString(), QStringLiteral("zh-CN"));

    AppSettings::instance().setDeveloperMode(false);
    AppSettings::instance().setLanguage(QStringLiteral("en"));
    EXPECT_FALSE(developer->isChecked());
    EXPECT_EQ(language->currentData().toString(), QStringLiteral("en"));
}

TEST_F(SettingsApplyContractTest, SearchNavigationPreservesDraftsAndCommittedSettings)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("settingsSearchField"));
    auto* empty = panel.findChild<QLabel*>(QStringLiteral("settingsSearchEmptyState"));
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("aiConnectionTestLabel"));
    auto* page = panel.findChild<AIAgentPage*>();
    auto* appearance = panel.findChild<AppearancePage*>();
    ASSERT_NE(search, nullptr);
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(appearance, nullptr);

    panel.show();
    ASSERT_TRUE(panel.showPage(page->pageId()));
    QCoreApplication::processEvents();
    const QString draftUrl = QStringLiteral("https://draft-search-value.example/v1");
    const QString draftKey = QStringLiteral("draft-search-secret-token");
    developer->setChecked(true);
    url->setText(draftUrl);
    key->setText(draftKey);
    ASSERT_TRUE(page->hasPendingChanges());

    search->setText(appearance->categoryLabel());
    EXPECT_EQ(panel.currentPageId(), appearance->pageId());
    EXPECT_TRUE(appearance->isVisible());
    for (const QString& query : {draftUrl, draftKey, secret_}) {
        search->setText(query);
        EXPECT_TRUE(empty->isVisible());
    }
    search->clear();
    EXPECT_FALSE(empty->isVisible());
    search->setText(appearance->categoryLabel());
    ASSERT_TRUE(panel.showPage(page->pageId()));

    EXPECT_EQ(panel.currentPageId(), page->pageId());
    EXPECT_TRUE(page->isVisible());
    EXPECT_TRUE(url->isVisible());
    EXPECT_TRUE(key->isVisible());
    EXPECT_TRUE(status->isHidden());
    EXPECT_EQ(url->text(), draftUrl);
    EXPECT_EQ(key->text(), draftKey);
    EXPECT_TRUE(page->hasPendingChanges());
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == originalConfig_);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_TRUE(AppSettings::instance().developerMode());
    EXPECT_TRUE(developer->isChecked());
}

TEST_F(SettingsApplyContractTest, DiscardPendingConnectionPreservesImmediatePermission)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* permission = panel.findChild<QComboBox*>(QStringLiteral("aiPermissionModeCombo"));
    auto* page = panel.findChild<AIAgentPage*>();
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(permission, nullptr);
    ASSERT_NE(page, nullptr);

    url->setText(QStringLiteral("https://unsaved.example/v1"));
    key->setText(QStringLiteral("unsaved-key"));
    permission->setCurrentIndex(permission->findData(QStringLiteral("read_only")));
    ASSERT_TRUE(page->hasPendingChanges());
    panel.discardConnectionChanges();

    auto expected = originalConfig_;
    expected.permissionProfile = AIPermissionMode::ReadOnly;
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == expected);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_EQ(url->text(), originalConfig_.baseUrl);
    EXPECT_EQ(key->text(), secret_);
    EXPECT_FALSE(page->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, ConnectionCommitPreservesLatestIndependentSettings)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* model = panel.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("aiConnectionSaveButton"));
    auto* permission = panel.findChild<QComboBox*>(QStringLiteral("aiPermissionModeCombo"));
    ASSERT_NE(url, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);
    ASSERT_NE(permission, nullptr);

    url->setText(QStringLiteral("https://replacement.example/v1"));
    model->setText(QStringLiteral("replacement-model"));
    key->setText(QStringLiteral("replacement-key"));
    auto expected = originalConfig_;
    expected.permissionProfile = AIPermissionMode::ReadOnly;
    expected.maxTokens = 2048;
    AIAgentConfigStore().save(expected);
    ASSERT_TRUE(save->isEnabled());
    save->click();

    expected.baseUrl = url->text();
    expected.model = model->text();
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == expected);
    EXPECT_EQ(secret_, QStringLiteral("replacement-key"));
    EXPECT_EQ(permission->currentData().toString(), QStringLiteral("read_only"));
    EXPECT_FALSE(panel.findChild<AIAgentPage*>()->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, ThemesSharePaletteEditsAndSelectTheirDefaultOnSwitch)
{
    const QString firstId = QStringLiteral("theme.custom");
    const QDir package(root_.filePath(firstId));
    ASSERT_TRUE(package.mkpath(QStringLiteral("visuals")));
    const QMap<QString, QByteArray> files{
        {QStringLiteral("manifest.json"), R"json({
            "schemaVersion":6,"id":"theme.custom","displayName":"Custom Theme","version":"1.0.0",
            "compatibility":{"harkenVersion":">=0.1.0 <1.0.0","capabilities":["curve-v1","meter-v1","waveform-v1"]},
            "entries":{"tokens":"tokens.json","visuals":{"curve":"visuals/curve.json","meter":"visuals/meter.json","waveform":"visuals/waveform.json"},"assetMap":"assets.json"}
        })json"},
        {QStringLiteral("tokens.json"), R"json({
            "colors":{"text.primary":"#ff203040"},
            "metrics":{"table.rowHeight":49},
            "fonts":{"body":{"pointSize":15}}
        })json"},
        {QStringLiteral("visuals/curve.json"), R"json({"colors":{}})json"},
        {QStringLiteral("visuals/meter.json"), R"json({"colors":{}})json"},
        {QStringLiteral("visuals/waveform.json"), R"json({
            "colors":{"waveform.selectionHighlight":"#a0556677"},
            "metrics":{"waveform.colorMap.offset0":0.22,"waveform.multiBand.alpha":0.35}
        })json"},
        {QStringLiteral("assets.json"), R"json({"assets":{}})json"}
    };
    for (auto entry = files.cbegin(); entry != files.cend(); ++entry) {
        QFile file(package.filePath(entry.key()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(entry.value()), entry.value().size());
    }

    PreferencesPanel panel(secretStore(), *theme_);
    auto* themes = panel.findChild<QComboBox*>(QStringLiteral("appearanceThemeCombo"));
    auto* presets = panel.findChild<QComboBox*>(QStringLiteral("appearanceColorPresets"));
    auto* colors = panel.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
    auto* reset = panel.findChild<QPushButton*>(QStringLiteral("appearanceColorReset"));
    auto* resetAll = panel.findChild<QPushButton*>(QStringLiteral("appearanceColorsReset"));
    ASSERT_NE(themes, nullptr);
    ASSERT_NE(presets, nullptr);
    ASSERT_NE(colors, nullptr);
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(resetAll, nullptr);
    const QString builtinId = builtinThemeId();
    const QString customPaletteId = defaultPaletteId(firstId);
    const QString amberId = builtinAmberPaletteId();
    const QString primary = QStringLiteral("text.primary");
    const QString secondary = QStringLiteral("text.secondary");
    const QColor firstColor(18, 52, 86, 128);
    const QColor secondColor(135, 114, 89, 192);
    const auto builtin = theme_->snapshot();
    QSet<QString> paletteIds;
    for (int i = 0; i < presets->count(); ++i)
        paletteIds.insert(presets->itemData(i).toString());
    EXPECT_TRUE(paletteIds.contains(amberId));
    EXPECT_TRUE(paletteIds.contains(customPaletteId));
    EXPECT_TRUE(paletteIds.contains(defaultPaletteId(builtinId)));
    ASSERT_TRUE(selectTheme(*themes, firstId));
    ASSERT_EQ(theme_->currentThemeId(), firstId);
    EXPECT_EQ(theme_->currentPaletteId(), customPaletteId);
    const auto defaults = theme_->snapshot();
    EXPECT_FLOAT_EQ(defaults->domains.waveform.colorMapMode.offset[0], 0.22f);
    EXPECT_FLOAT_EQ(defaults->domains.waveform.multiBandMode.alpha, 0.35f);
    const auto before = storedValues(*themeStorage_);
    const auto palette = theme_->loadPalette(amberId);
    ASSERT_TRUE(palette.ok());
    ASSERT_NE(defaults->domains.waveform.selectionHighlight,
              palette.palette->snapshot->domains.waveform.selectionHighlight);
    ASSERT_TRUE(applyColorPreset(*presets, amberId));
    EXPECT_EQ(theme_->currentThemeId(), firstId);
    EXPECT_EQ(themes->currentData().toString(), firstId);
    EXPECT_EQ(theme_->currentPaletteId(), amberId);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, palette.palette->colors.value(primary));
    EXPECT_EQ(theme_->snapshot()->domains.waveform.selectionHighlight,
              palette.palette->snapshot->domains.waveform.selectionHighlight);
    EXPECT_EQ(theme_->snapshot()->domains.waveform.colorMapMode.offset,
              palette.palette->snapshot->domains.waveform.colorMapMode.offset);
    EXPECT_FLOAT_EQ(theme_->snapshot()->domains.waveform.multiBandMode.alpha,
                    palette.palette->snapshot->domains.waveform.multiBandMode.alpha);
    EXPECT_EQ(theme_->snapshot()->ui.metrics.table.rowHeight, 49);
    EXPECT_EQ(theme_->snapshot()->ui.typography.body.pointSize, 15);

    ASSERT_TRUE(selectColor(*colors, primary));
    ASSERT_TRUE(finishColorEdit(panel, *colors, firstColor));
    ASSERT_TRUE(selectColor(*colors, secondary));
    ASSERT_TRUE(finishColorEdit(panel, *colors, secondColor));
    EXPECT_EQ(theme_->appearance().colors.value(amberId).value(primary), firstColor);
    EXPECT_EQ(theme_->appearance().colors.value(amberId).value(secondary), secondColor);
    EXPECT_NE(storedValues(*themeStorage_), before);
    ASSERT_TRUE(selectTheme(*themes, builtinId));
    EXPECT_EQ(theme_->currentPaletteId(), defaultPaletteId(builtinId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, builtin->ui.colors.textPrimary);
    QSet<QString> switchedPaletteIds;
    for (int i = 0; i < presets->count(); ++i)
        switchedPaletteIds.insert(presets->itemData(i).toString());
    EXPECT_EQ(switchedPaletteIds, paletteIds);
    ASSERT_TRUE(applyColorPreset(*presets, amberId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, firstColor);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textSecondary, secondColor);
    EXPECT_EQ(theme_->snapshot()->ui.metrics.table.rowHeight, builtin->ui.metrics.table.rowHeight);
    EXPECT_EQ(theme_->snapshot()->ui.typography.body.pointSize, builtin->ui.typography.body.pointSize);

    ASSERT_TRUE(selectTheme(*themes, firstId));
    EXPECT_EQ(theme_->currentPaletteId(), customPaletteId);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, defaults->ui.colors.textPrimary);
    EXPECT_EQ(theme_->snapshot()->domains.waveform.selectionHighlight,
              defaults->domains.waveform.selectionHighlight);
    EXPECT_EQ(theme_->snapshot()->domains.waveform.colorMapMode.offset,
              defaults->domains.waveform.colorMapMode.offset);
    EXPECT_FLOAT_EQ(theme_->snapshot()->domains.waveform.multiBandMode.alpha,
                    defaults->domains.waveform.multiBandMode.alpha);
    ASSERT_TRUE(applyColorPreset(*presets, amberId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, firstColor);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textSecondary, secondColor);
    EXPECT_EQ(theme_->snapshot()->ui.metrics.table.rowHeight, 49);
    EXPECT_EQ(theme_->snapshot()->ui.typography.body.pointSize, 15);
    ASSERT_TRUE(selectColor(*colors, primary));
    ASSERT_TRUE(reset->isEnabled());
    reset->click();
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, palette.palette->colors.value(primary));
    EXPECT_FALSE(theme_->appearance().colors.value(amberId).contains(primary));
    EXPECT_EQ(theme_->appearance().colors.value(amberId).value(secondary), secondColor);
    ASSERT_TRUE(resetAll->isEnabled());
    resetAll->click();
    EXPECT_FALSE(theme_->appearance().colors.contains(amberId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textSecondary, palette.palette->colors.value(secondary));
    EXPECT_EQ(theme_->currentThemeId(), firstId);
    EXPECT_EQ(theme_->currentPaletteId(), amberId);

    ASSERT_TRUE(selectTheme(*themes, builtinId));
    ASSERT_TRUE(applyColorPreset(*presets, customPaletteId));
    EXPECT_EQ(theme_->currentThemeId(), builtinId);
    EXPECT_EQ(theme_->snapshot()->domains.waveform.selectionHighlight,
              defaults->domains.waveform.selectionHighlight);
    EXPECT_EQ(theme_->snapshot()->domains.waveform.colorMapMode.offset,
              defaults->domains.waveform.colorMapMode.offset);
    EXPECT_FLOAT_EQ(theme_->snapshot()->domains.waveform.multiBandMode.alpha,
                    defaults->domains.waveform.multiBandMode.alpha);
    EXPECT_EQ(theme_->snapshot()->ui.metrics.table.rowHeight, builtin->ui.metrics.table.rowHeight);
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.currentThemeId(), builtinId);
    EXPECT_EQ(restored.currentPaletteId(), customPaletteId);
    EXPECT_EQ(restored.snapshot()->domains.waveform.selectionHighlight,
              defaults->domains.waveform.selectionHighlight);
    ASSERT_TRUE(restored.selectTheme(firstId).ok());
    EXPECT_EQ(restored.currentPaletteId(), customPaletteId);
    EXPECT_EQ(restored.snapshot()->ui.metrics.table.rowHeight, 49);
    EXPECT_EQ(restored.snapshot()->ui.typography.body.pointSize, 15);
}

TEST_F(SettingsApplyContractTest, AppearanceFollowsExternalCommittedPaletteAndColors)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* themes = panel.findChild<QComboBox*>(QStringLiteral("appearanceThemeCombo"));
    auto* presets = panel.findChild<QComboBox*>(QStringLiteral("appearanceColorPresets"));
    auto* colors = panel.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
    ASSERT_NE(themes, nullptr);
    ASSERT_NE(presets, nullptr);
    ASSERT_NE(colors, nullptr);
    ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));

    auto appearance = theme_->appearance();
    appearance.paletteId = builtinAmberPaletteId();
    const QColor externalColor(63, 95, 127, 160);
    appearance.colors[appearance.paletteId].insert(QStringLiteral("text.primary"), externalColor);
    ASSERT_TRUE(theme_->applyAppearance(appearance).ok());

    EXPECT_EQ(themes->currentData().toString(), appearance.themeId);
    EXPECT_EQ(presets->currentData().toString(), appearance.paletteId);
    EXPECT_EQ(QColor(colors->currentItem()->text(1)), externalColor);
}

TEST_F(SettingsApplyContractTest, ColorPickerPreviewRequiresAcceptance)
{
    const auto committed = theme_->appearance();
    const auto original = theme_->snapshot();
    const auto before = storedValues(*themeStorage_);
    const QColor previewColor(27, 81, 143, 176);
    {
        AppearancePage page(*theme_);
        page.load();
        auto* colors = page.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
        ASSERT_NE(colors, nullptr);
        ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));
        auto* picker = openColorEditor(page, *colors);
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);

        auto* hue = picker->findChild<QSpinBox*>(QStringLiteral("colorHue"));
        auto* saturation = picker->findChild<QSpinBox*>(QStringLiteral("colorSaturation"));
        auto* value = picker->findChild<QSpinBox*>(QStringLiteral("colorValue"));
        auto* red = picker->findChild<QSpinBox*>(QStringLiteral("colorRed"));
        auto* green = picker->findChild<QSpinBox*>(QStringLiteral("colorGreen"));
        auto* blue = picker->findChild<QSpinBox*>(QStringLiteral("colorBlue"));
        auto* alphaControl = picker->findChild<LinearSliderPresenter*>(QStringLiteral("colorAlphaControl"));
        auto* alphaBinding = picker->findChild<ParameterBinding*>(QStringLiteral("colorAlphaBinding"));
        for (auto* field : {hue, saturation, value, red, green, blue})
            ASSERT_NE(field, nullptr);
        ASSERT_NE(alphaControl, nullptr);
        ASSERT_NE(alphaBinding, nullptr);
        auto* alpha = alphaControl->valueEditor();
        auto* alphaSlider = alphaControl->slider();
        ASSERT_NE(alpha, nullptr);
        ASSERT_NE(alphaSlider, nullptr);

        alpha->setValue(91);
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 91));
        EXPECT_EQ(alphaSlider->value(), 91);
        alphaSlider->setValue(64);
        EXPECT_EQ(alpha->value(), 64);
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 64));

        alphaSlider->setSliderDown(true);
        alphaSlider->setValue(120);
        EXPECT_TRUE(alphaBinding->isEditing());
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, QColor(27, 81, 143, 120));
        alphaSlider->setValue(160);
        EXPECT_TRUE(alphaBinding->isEditing());
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 160));
        alphaBinding->cancelEdit();
        alphaSlider->setSliderDown(false);
        EXPECT_EQ(alpha->value(), 64);
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 64));
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, picker->currentColor());
        EXPECT_EQ(storedValues(*themeStorage_), before);

        red->setValue(255);
        green->setValue(0);
        blue->setValue(0);
        EXPECT_EQ(hue->value(), 0);
        EXPECT_EQ(saturation->value(), 100);
        EXPECT_EQ(value->value(), 100);
        saturation->setValue(0);
        hue->setValue(240);
        EXPECT_EQ(picker->currentColor(), QColor(255, 255, 255, 64));
        saturation->setValue(100);
        EXPECT_EQ(picker->currentColor(), QColor(0, 0, 255, 64));
        EXPECT_EQ(red->value(), 0);
        EXPECT_EQ(green->value(), 0);
        EXPECT_EQ(blue->value(), 255);
        value->setValue(0);
        hue->setValue(120);
        EXPECT_EQ(picker->currentColor(), QColor(0, 0, 0, 64));
        value->setValue(100);
        EXPECT_EQ(picker->currentColor(), QColor(0, 255, 0, 64));
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, picker->currentColor());
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);

        picker->reject();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);
        QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);

        picker = openColorEditor(page, *colors);
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        picker->accept();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
        EXPECT_EQ(theme_->appearance().colors.value(committed.paletteId)
                      .value(QStringLiteral("text.primary")), previewColor);
        EXPECT_NE(storedValues(*themeStorage_), before);
    }
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, previewColor);
}

TEST_F(SettingsApplyContractTest, FailedPickerCommitAndPageDestructionRestoreCommittedAppearance)
{
    auto storage = std::make_unique<WritableSettingsStorage>();
    auto* stored = storage.get();
    ThemeManager manager(root_.path(), std::move(storage));
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    const auto before = storedValues(*stored);
    const QColor previewColor(41, 97, 151, 192);
    {
        AppearancePage page(manager);
        page.load();
        auto* colors = page.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
        auto* error = page.findChild<QLabel*>(QStringLiteral("appearanceColorError"));
        ASSERT_NE(colors, nullptr);
        ASSERT_NE(error, nullptr);
        ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));
        auto* picker = openColorEditor(page, *colors);
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        ASSERT_EQ(manager.snapshot()->ui.colors.textPrimary, previewColor);
        stored->writable = false;
        picker->accept();

        EXPECT_EQ(manager.appearance(), committed);
        EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
        EXPECT_EQ(QColor(colors->currentItem()->text(1)), original->ui.colors.textPrimary);
        EXPECT_EQ(storedValues(*stored), before);
        EXPECT_FALSE(error->text().isEmpty());
        QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);

        picker = openColorEditor(page, *colors);
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        ASSERT_EQ(manager.snapshot()->ui.colors.textPrimary, previewColor);
    }
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
    EXPECT_EQ(storedValues(*stored), before);
}

}
