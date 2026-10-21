
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

class QComboBox;

namespace VNX {

class ThemeManager;

inline constexpr const char* kSettingsPageAppearance = "appearance";

class AppearancePage : public SettingsPage
{
    Q_OBJECT

public:
    explicit AppearancePage(ThemeManager& themeManager, QWidget* parent = nullptr);

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
    void populateLanguageCombo();

    ThemeManager* themeManager_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;

    QString baselineThemeId_;
    QString baselineLanguage_;

    QString revertThemeId_;
    QString revertLanguage_;
    bool canRevert_ = false;
};

}
