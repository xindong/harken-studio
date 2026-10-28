
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

protected:
    void showEvent(QShowEvent* event) override;

private:
    bool applyTheme();
    void applyLanguage();
    void populateLanguageCombo();

    ThemeManager* themeManager_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
};

}
