
#include "Dialogs/Settings/PreferencesDialog.h"

#include "Core/DialogManager.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/Pages/AppearancePage.h"
#include "Dialogs/Settings/Pages/ExtensionPage.h"
#include "Dialogs/Settings/Pages/LayoutPage.h"
#include "Dialogs/Settings/Pages/ShortcutsPage.h"
#include "Platform/SecretStore.h"

namespace VNX {

namespace {

PreferencesDialog::SecretStoreAccess productionSecretStoreAccess()
{
    PreferencesDialog::SecretStoreAccess access;
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

PreferencesDialog::PreferencesDialog(QWidget* parent, SaveCurrentLayout saveCurrentLayout)
    : PreferencesDialog(productionSecretStoreAccess(), ThemeManager::instance(), parent,
                        std::move(saveCurrentLayout))
{
}

PreferencesDialog::PreferencesDialog(SecretStoreAccess secretStore, QWidget* parent)
    : PreferencesDialog(std::move(secretStore), ThemeManager::instance(), parent)
{
}

PreferencesDialog::PreferencesDialog(SecretStoreAccess secretStore,
                                     ThemeManager& themeManager,
                                     QWidget* parent, SaveCurrentLayout saveCurrentLayout)
    : SettingsShellDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    addPage(new AppearancePage(themeManager, this));
    addPage(new ExtensionPage(this));
    addPage(new ShortcutsPage(this));

    auto* layoutPage = new LayoutPage(this, std::move(saveCurrentLayout));
    connect(layoutPage, &LayoutPage::layoutRenamed,
            this, &PreferencesDialog::layoutRenamed);
    connect(layoutPage, &LayoutPage::layoutDeleted,
            this, &PreferencesDialog::layoutDeleted);
    addPage(layoutPage);

    auto* aiPage = new AIAgentPage(std::move(secretStore), this);
    connect(aiPage, &AIAgentPage::aiAgentConfigApplied,
            this, &PreferencesDialog::aiAgentConfigApplied);
    addPage(aiPage);

    loadPages();
    DialogManager::instance().registerDialog(QStringLiteral("preferences"), this);
}

}
