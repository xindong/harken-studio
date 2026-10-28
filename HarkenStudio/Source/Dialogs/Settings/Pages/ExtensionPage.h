#pragma once

#include "Dialogs/Settings/SettingsPage.h"

class QCheckBox;

namespace VNX {

inline constexpr const char* kSettingsPageExtension = "extension";

class ExtensionPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ExtensionPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;

private:
    QCheckBox* developerModeCheck_ = nullptr;
};

}
