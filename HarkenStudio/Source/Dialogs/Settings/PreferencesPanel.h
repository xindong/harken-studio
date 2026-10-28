
#pragma once

#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/Pages/LayoutPage.h"
#include "Dialogs/Settings/SettingsPanel.h"

namespace VNX {

class ThemeManager;

class PreferencesPanel : public SettingsPanel
{
    Q_OBJECT

public:
    using SecretStoreAccess = AIAgentPage::SecretStoreAccess;
    using SaveCurrentLayout = LayoutPage::SaveCurrentLayout;

    explicit PreferencesPanel(QWidget* parent = nullptr, SaveCurrentLayout saveCurrentLayout = {});
    explicit PreferencesPanel(SecretStoreAccess secretStore, QWidget* parent = nullptr);
    PreferencesPanel(SecretStoreAccess secretStore, ThemeManager& themeManager,
                      QWidget* parent = nullptr, SaveCurrentLayout saveCurrentLayout = {});
    void discardConnectionChanges();
    void refreshLayouts();

signals:
    void aiAgentConfigApplied();
    void layoutRenamed(const QString& oldName, const QString& newName);
    void layoutDeleted(const QString& name);

private:
    AIAgentPage* aiPage_ = nullptr;
    LayoutPage* layoutPage_ = nullptr;
};

}
