
#pragma once

#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/SettingsShellDialog.h"

namespace VNX {

class ThemeManager;

class PreferencesDialog : public SettingsShellDialog
{
    Q_OBJECT

public:
    using SecretStoreAccess = AIAgentPage::SecretStoreAccess;

    explicit PreferencesDialog(QWidget* parent = nullptr);
    explicit PreferencesDialog(SecretStoreAccess secretStore, QWidget* parent = nullptr);
    PreferencesDialog(SecretStoreAccess secretStore, ThemeManager& themeManager,
                      QWidget* parent = nullptr);

signals:
    void aiAgentConfigApplied();
    void layoutRenamed(const QString& oldName, const QString& newName);
    void layoutDeleted(const QString& name);
};

}
