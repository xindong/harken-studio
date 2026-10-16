
#pragma once

#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/Pages/LayoutPage.h"
#include "Dialogs/Settings/SettingsShellDialog.h"

namespace VNX {

class ThemeManager;

class PreferencesDialog : public SettingsShellDialog
{
    Q_OBJECT

public:
    using SecretStoreAccess = AIAgentPage::SecretStoreAccess;
    using SaveCurrentLayout = LayoutPage::SaveCurrentLayout;

    explicit PreferencesDialog(QWidget* parent = nullptr, SaveCurrentLayout saveCurrentLayout = {});
    explicit PreferencesDialog(SecretStoreAccess secretStore, QWidget* parent = nullptr);
    PreferencesDialog(SecretStoreAccess secretStore, ThemeManager& themeManager,
                      QWidget* parent = nullptr, SaveCurrentLayout saveCurrentLayout = {});

signals:
    void aiAgentConfigApplied();
    void layoutRenamed(const QString& oldName, const QString& newName);
    void layoutDeleted(const QString& name);
};

}
