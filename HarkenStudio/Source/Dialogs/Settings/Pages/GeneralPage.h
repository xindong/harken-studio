
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

class QCheckBox;
class QComboBox;

namespace VNX {

class ThemeManager;

inline constexpr const char* kSettingsPageGeneral = "general";

class GeneralPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit GeneralPage(ThemeManager& themeManager, QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;
    void revert() override;

private:
    void rememberBaseline();
    bool applyTheme();
    void applyLanguage();
    void applyDeveloperMode();
    void populateLanguageCombo();

    ThemeManager* themeManager_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QCheckBox* developerModeCheck_ = nullptr;

    QString baselineThemeId_;
    QString baselineLanguage_;
    bool baselineDeveloperMode_ = false;

    QString revertThemeId_;
    QString revertLanguage_;
    bool revertDeveloperMode_ = false;
    bool canRevert_ = false;
};

}
