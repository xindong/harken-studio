
#include "Dialogs/Settings/PreferencesPanel.h"

#include "Core/ThemeManager.h"
#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/Pages/AppearancePage.h"
#include "Dialogs/Settings/Pages/ExtensionPage.h"
#include "Dialogs/Settings/Pages/LayoutPage.h"
#include "Dialogs/Settings/Pages/ShortcutsPage.h"
#include "Platform/SecretStore.h"

namespace VNX {

namespace {

PreferencesPanel::SecretStoreAccess productionSecretStoreAccess()
{
    PreferencesPanel::SecretStoreAccess access;
    access.load = [](const QString& service, const QString& account) {
        return Harken::Platform::SecretStore::load(service, account);
    };
    access.store = [](const QString& service, const QString& account,
                      const QString& secret) {
        return Harken::Platform::SecretStore::store(service, account, secret);
    };
    access.erase = [](const QString& service, const QString& account) {
        return Harken::Platform::SecretStore::erase(service, account);
    };
    return access;
}

}

PreferencesPanel::PreferencesPanel(QWidget* parent, SaveCurrentLayout saveCurrentLayout)
    : PreferencesPanel(productionSecretStoreAccess(), ThemeManager::instance(), parent,
                        std::move(saveCurrentLayout))
{
}

PreferencesPanel::PreferencesPanel(SecretStoreAccess secretStore, QWidget* parent)
    : PreferencesPanel(std::move(secretStore), ThemeManager::instance(), parent)
{
}

PreferencesPanel::PreferencesPanel(SecretStoreAccess secretStore,
                                     ThemeManager& themeManager,
                                     QWidget* parent, SaveCurrentLayout saveCurrentLayout)
    : SettingsPanel(parent)
{
    setObjectName(QStringLiteral("PreferencesPanel"));
    setWindowTitle(tr("Preferences"));

    addPage(new AppearancePage(themeManager, this));
    addPage(new ShortcutsPage(this));

    layoutPage_ = new LayoutPage(this, std::move(saveCurrentLayout));
    connect(layoutPage_, &LayoutPage::layoutRenamed,
            this, &PreferencesPanel::layoutRenamed);
    connect(layoutPage_, &LayoutPage::layoutDeleted,
            this, &PreferencesPanel::layoutDeleted);
    addPage(layoutPage_);

    aiPage_ = new AIAgentPage(std::move(secretStore), this);
    connect(aiPage_, &AIAgentPage::aiAgentConfigApplied,
            this, &PreferencesPanel::aiAgentConfigApplied);
    addPage(aiPage_);
    addPage(new ExtensionPage(this));

    loadPages();
}

void PreferencesPanel::discardConnectionChanges()
{
    aiPage_->discardConnectionChanges();
}

void PreferencesPanel::refreshLayouts()
{
    layoutPage_->load();
}

}
