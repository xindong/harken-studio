#include "Core/AI/AIAgentConfig.h"
#include "Core/AppSettings.h"
#include "Core/ColorPalette.h"
#include "Core/HarkenStyle.h"
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
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QScopeGuard>
#include <QSlider>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTimer>
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

bool waitForPreview(VNX::ThemeManager& manager, const QColor& color)
{
    if (manager.snapshot()->ui.colors.textPrimary == color) return true;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&manager, &VNX::ThemeManager::appearanceChanged, &loop, [&] {
        if (manager.snapshot()->ui.colors.textPrimary == color) loop.quit();
    });
    timeout.start(1000);
    loop.exec();
    return manager.snapshot()->ui.colors.textPrimary == color;
}

void settlePreviewEvents()
{
    QEventLoop loop;
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
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

TEST_F(SettingsApplyContractTest, ThemePalettesAndRolesKeepEditsIsolatedAcrossSwitchResetAndRestart)
{
    const QString customId = QStringLiteral("theme.custom");
    const QDir package(root_.filePath(customId));
    ASSERT_TRUE(package.mkpath(QStringLiteral("visuals")));
    const QMap<QString, QByteArray> files{
        {QStringLiteral("manifest.json"), R"json({
            "schemaVersion":7,"id":"theme.custom","displayName":"Custom Theme","version":"1.0.0",
            "compatibility":{"harkenVersion":">=0.1.0 <1.0.0","capabilities":["curve-v1","meter-v1","waveform-v1"]},
            "entries":{"tokens":"tokens.json","colors":"colors.json","visuals":{"curve":"visuals/curve.json","meter":"visuals/meter.json","waveform":"visuals/waveform.json"},"assetMap":"assets.json"}
        })json"},
        {QStringLiteral("tokens.json"), R"json({
            "colors":{},"metrics":{"table.rowHeight":49},"fonts":{"body":{"pointSize":15}}
        })json"},
        {QStringLiteral("colors.json"), R"json({
            "defaultPalette":"dark",
            "roles":[
                {"key":"ink","group":"Surfaces","label":"Ink","description":"Reading text.","bindings":[{"target":"text.primary"}]},
                {"key":"grid","group":"Canvas","label":"Grid","description":"Independent curve grid.","bindings":[{"target":"curve.grid"}]}
            ],
            "palettes":[
                {"id":"dark","displayName":"Night","colorScheme":"dark","colors":{"ink":"#e0d0c0","grid":"#485868"}},
                {"id":"paper","displayName":"Paper","colorScheme":"light","colors":{"ink":"#203040","grid":"#98a8b8"}}
            ]
        })json"},
        {QStringLiteral("visuals/curve.json"), R"json({"colors":{}})json"},
        {QStringLiteral("visuals/meter.json"), R"json({"colors":{}})json"},
        {QStringLiteral("visuals/waveform.json"), R"json({"colors":{}})json"},
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
    const QString darkId = QStringLiteral("dark");
    const QString lightId = QStringLiteral("light");
    const QString paperId = QStringLiteral("paper");
    const QString ink = QStringLiteral("ink");
    const QString grid = QStringLiteral("grid");
    const QColor editedInk(18, 52, 86, 128);
    const QColor editedGrid(135, 114, 89, 192);
    const auto builtin = theme_->snapshot();
    EXPECT_GE(presets->findData(darkId), 0);
    EXPECT_GE(presets->findData(lightId), 0);
    EXPECT_LT(presets->findData(paperId), 0);
    ASSERT_TRUE(selectTheme(*themes, customId));
    ASSERT_EQ(theme_->currentThemeId(), customId);
    EXPECT_EQ(theme_->currentPaletteId(), darkId);
    EXPECT_LT(presets->findData(lightId), 0);
    EXPECT_GE(presets->findData(paperId), 0);
    EXPECT_FALSE(selectColor(*colors, QStringLiteral("text.primary")));
    const auto defaults = theme_->snapshot();
    ASSERT_TRUE(selectColor(*colors, ink));
    ASSERT_TRUE(finishColorEdit(panel, *colors, editedInk));
    ASSERT_TRUE(selectColor(*colors, grid));
    ASSERT_TRUE(finishColorEdit(panel, *colors, editedGrid));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, editedInk);
    EXPECT_EQ(theme_->snapshot()->domains.curve.colors.grid, editedGrid);
    EXPECT_EQ(theme_->snapshot()->ui.metrics.table.rowHeight, 49);
    EXPECT_EQ(theme_->snapshot()->ui.typography.body.pointSize, 15);

    ASSERT_TRUE(applyColorPreset(*presets, paperId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, QColor("#203040"));
    EXPECT_EQ(theme_->snapshot()->colorScheme, ThemeColorScheme::Light);
    ASSERT_TRUE(applyColorPreset(*presets, darkId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, editedInk);
    EXPECT_EQ(theme_->snapshot()->domains.curve.colors.grid, editedGrid);

    ASSERT_TRUE(selectTheme(*themes, builtinId));
    EXPECT_EQ(theme_->currentPaletteId(), darkId);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, builtin->ui.colors.textPrimary);
    EXPECT_FALSE(selectColor(*colors, ink));
    ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));
    const QColor builtinEdit(61, 83, 109);
    ASSERT_TRUE(finishColorEdit(panel, *colors, builtinEdit));
    ASSERT_TRUE(selectTheme(*themes, customId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, editedInk);
    EXPECT_EQ(theme_->snapshot()->domains.curve.colors.grid, editedGrid);
    ASSERT_TRUE(selectColor(*colors, ink));
    ASSERT_TRUE(reset->isEnabled());
    reset->click();
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, defaults->ui.colors.textPrimary);
    EXPECT_EQ(theme_->snapshot()->domains.curve.colors.grid, editedGrid);
    ASSERT_TRUE(resetAll->isEnabled());
    resetAll->click();
    EXPECT_EQ(theme_->snapshot()->domains.curve.colors.grid, defaults->domains.curve.colors.grid);
    EXPECT_FALSE(theme_->appearance().colors.contains(customId));
    EXPECT_EQ(theme_->appearance().colors.value(builtinId).value(darkId)
                  .value(QStringLiteral("text.primary")), builtinEdit);
    ASSERT_TRUE(applyColorPreset(*presets, paperId));
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.currentThemeId(), customId);
    EXPECT_EQ(restored.currentPaletteId(), paperId);
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, QColor("#203040"));
    EXPECT_EQ(restored.snapshot()->ui.metrics.table.rowHeight, 49);
    ASSERT_TRUE(restored.selectTheme(builtinId).ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, builtinEdit);
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
    auto* picker = openColorEditor(panel, *colors);
    ASSERT_NE(picker, nullptr);
    picker->setCurrentColor(QColor(11, 47, 101, 192));

    auto appearance = theme_->appearance();
    appearance.paletteId = QStringLiteral("light");
    const QColor externalColor(63, 95, 127, 160);
    appearance.colors[appearance.themeId][appearance.paletteId].insert(QStringLiteral("text.primary"), externalColor);
    ASSERT_TRUE(theme_->applyAppearance(appearance).ok());
    settlePreviewEvents();

    EXPECT_EQ(theme_->appearance(), appearance);
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, externalColor);
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
        picker->setCurrentColor(QColor(17, 43, 89, 128));
        picker->setCurrentColor(previewColor);
        EXPECT_EQ(picker->currentColor(), previewColor);
        ASSERT_TRUE(waitForPreview(*theme_, previewColor));
        const auto preview = theme_->snapshot();
        settlePreviewEvents();
        EXPECT_EQ(theme_->snapshot(), preview);
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
        ASSERT_TRUE(waitForPreview(*theme_, QColor(27, 81, 143, 120)));
        alphaSlider->setValue(160);
        EXPECT_TRUE(alphaBinding->isEditing());
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 160));
        alphaBinding->cancelEdit();
        alphaSlider->setSliderDown(false);
        EXPECT_EQ(alpha->value(), 64);
        EXPECT_EQ(picker->currentColor(), QColor(27, 81, 143, 64));
        ASSERT_TRUE(waitForPreview(*theme_, picker->currentColor()));
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
        ASSERT_TRUE(waitForPreview(*theme_, picker->currentColor()));
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);

        picker->setCurrentColor(QColor(59, 103, 149, 208));
        picker->reject();
        settlePreviewEvents();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);
        picker = openColorEditor(page, *colors);
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(QColor(19, 61, 127, 160));
        picker->setCurrentColor(previewColor);
        picker->accept();
        settlePreviewEvents();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
        EXPECT_EQ(theme_->appearance().colors.value(committed.themeId).value(committed.paletteId)
                      .value(QStringLiteral("text.primary")), previewColor);
        EXPECT_NE(storedValues(*themeStorage_), before);
    }
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, previewColor);
}

TEST_F(SettingsApplyContractTest, EveryPaletteRoleHasIdenticalPreviewAndCommittedPresentation)
{
    qApp->setStyle(new HarkenStyle(QStyleFactory::create(QStringLiteral("Fusion")), theme_.get()));
    const auto restoreStyle = qScopeGuard([] { qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion"))); });
    AppearancePage page(*theme_);
    page.load();
    page.show();
    settlePreviewEvents();
    auto* colors = page.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
    auto* presets = page.findChild<QComboBox*>(QStringLiteral("appearanceColorPresets"));
    ASSERT_NE(colors, nullptr);
    ASSERT_NE(presets, nullptr);
    const auto presentation = [&] {
        QMap<QString, QImage> images;
        for (int row = 0; row < colors->topLevelItemCount(); ++row) {
            const auto* item = colors->topLevelItem(row);
            const QString key = item->data(0, Qt::UserRole).toString();
            for (const int column : {1, 2}) {
                const auto icon = item->icon(column);
                const auto sizes = icon.availableSizes();
                for (const auto& size : sizes)
                    images.insert(key + QStringLiteral("/%1/%2x%3").arg(column).arg(size.width()).arg(size.height()),
                                  icon.pixmap(size).toImage());
            }
        }
        for (int index = 0; index < presets->count(); ++index)
            images.insert(QStringLiteral("preset/") + presets->itemData(index).toString(),
                          presets->itemData(index, Qt::UserRole + 1).value<QPixmap>().toImage());
        return images;
    };
    const auto original = theme_->appearance();
    for (const auto& palette : theme_->availablePalettes()) {
        auto committed = original;
        committed.paletteId = palette.id;
        const auto roles = theme_->colorRoles();
        for (const auto& role : roles) {
            SCOPED_TRACE((palette.id + QLatin1Char('/') + role.key).toStdString());
            ASSERT_TRUE(theme_->applyAppearance(committed).ok());
            settlePreviewEvents();
            ASSERT_TRUE(selectColor(*colors, role.key));
            const auto before = presentation();
            ASSERT_FALSE(before.isEmpty());
            const auto storage = storedValues(*themeStorage_);
            auto* picker = openColorEditor(page, *colors);
            ASSERT_NE(picker, nullptr);
            const auto generation = theme_->snapshot()->generation;
            picker->setCurrentColor(QColor(197, 43, 131, 149));
            settlePreviewEvents();
            ASSERT_GT(theme_->snapshot()->generation, generation);
            EXPECT_EQ(theme_->appearance(), committed);
            EXPECT_EQ(storedValues(*themeStorage_), storage);
            const auto preview = presentation();
            auto previewSnapshot = *theme_->snapshot();
            picker->accept();
            QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);
            settlePreviewEvents();
            auto appliedSnapshot = *theme_->snapshot();
            appliedSnapshot.generation = previewSnapshot.generation;
            EXPECT_EQ(appliedSnapshot, previewSnapshot);
            EXPECT_EQ(presentation(), preview);

            ASSERT_TRUE(theme_->applyAppearance(committed).ok());
            ASSERT_TRUE(selectColor(*colors, role.key));
            picker = openColorEditor(page, *colors);
            ASSERT_NE(picker, nullptr);
            picker->setCurrentColor(QColor(197, 43, 131, 149));
            settlePreviewEvents();
            EXPECT_EQ(presentation(), preview);
            picker->reject();
            QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);
            settlePreviewEvents();
            EXPECT_EQ(theme_->appearance(), committed);
            EXPECT_EQ(presentation(), before);
        }
    }
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
        ASSERT_TRUE(waitForPreview(manager, previewColor));
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
        ASSERT_TRUE(waitForPreview(manager, previewColor));
        picker->setCurrentColor(QColor(71, 113, 157, 224));
    }
    settlePreviewEvents();
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
    EXPECT_EQ(storedValues(*stored), before);
}

}
