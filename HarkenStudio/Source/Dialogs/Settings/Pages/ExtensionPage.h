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
    bool apply() override;
    bool isDirty() const override;
    void revert() override;

private:
    QCheckBox* developerModeCheck_ = nullptr;
    bool baselineDeveloperMode_ = false;
    bool revertDeveloperMode_ = false;
    bool canRevert_ = false;
};

}
