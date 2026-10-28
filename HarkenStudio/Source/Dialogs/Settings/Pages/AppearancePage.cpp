
#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"
#include "Core/ThemeManager.h"
#include "Core/ThemePackageParser.h"

#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QShowEvent>
#include <QSignalBlocker>

namespace VNX {

AppearancePage::AppearancePage(ThemeManager& themeManager, QWidget* parent)
    : SettingsPage(parent)
    , themeManager_(&themeManager)
{
    auto* body = makePageBody();

    auto* appearance = makeSection(tr("Appearance"));
    auto* appearanceForm = makeForm();
    auto* themeLabel = new QLabel(tr("Theme:"));
    themeCombo_ = new QComboBox();
    themeCombo_->setObjectName(QStringLiteral("appearanceThemeCombo"));
    const auto themeTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.theme"),
        {QDir::toNativeSeparators(themeManager_->userThemesRoot())});
    ContextInfo::tag(themeCombo_, themeTopic);
    ContextInfo::tag(themeLabel, themeTopic);
    themeLabel->setBuddy(themeCombo_);
    appearanceForm->addRow(themeLabel, themeCombo_);

    auto* languageLabel = new QLabel(tr("Information Language:"));
    languageCombo_ = new QComboBox();
    languageCombo_->setObjectName(QStringLiteral("appearanceLanguageCombo"));
    const auto languageTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.language"));
    ContextInfo::tag(languageCombo_, languageTopic);
    ContextInfo::tag(languageLabel, languageTopic);
    populateLanguageCombo();
    languageLabel->setBuddy(languageCombo_);
    appearanceForm->addRow(languageLabel, languageCombo_);
    static_cast<QVBoxLayout*>(appearance->layout())->addLayout(appearanceForm);

    body->addWidget(appearance);

    body->addStretch();

    connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::applyTheme);
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::applyLanguage);
    connect(themeManager_, &ThemeManager::themeChanged, this, &AppearancePage::load);
    connect(&AppSettings::instance(), &AppSettings::languageChanged, this, &AppearancePage::load);
}

QString AppearancePage::pageId() const
{
    return QString::fromLatin1(kSettingsPageAppearance);
}

QString AppearancePage::categoryLabel() const
{
    return tr("Appearance");
}

void AppearancePage::populateLanguageCombo()
{
    languageCombo_->addItem(tr("English"), QStringLiteral("en"));
    languageCombo_->addItem(tr("Chinese (Simplified)"), QStringLiteral("zh-CN"));
}

void AppearancePage::load()
{
    themeManager_->refreshThemes();
    {
        const QSignalBlocker blocker(themeCombo_);
        themeCombo_->clear();
        for (const ThemePackageDescriptor& theme : themeManager_->availableThemes())
        {
            const QString label = theme.builtin
                ? tr("%1 (Built-in)").arg(theme.displayName)
                : theme.displayName;
            themeCombo_->addItem(label, theme.id);
            const int index = themeCombo_->count() - 1;
            themeCombo_->setItemData(
                index, tr("ID: %1\nVersion: %2").arg(theme.id, theme.version),
                Qt::ToolTipRole);
        }

        int currentIndex = themeCombo_->findData(themeManager_->currentThemeId());
        if (currentIndex < 0)
        {
            themeCombo_->addItem(themeManager_->currentThemeId(),
                                 themeManager_->currentThemeId());
            currentIndex = themeCombo_->count() - 1;
        }
        themeCombo_->setCurrentIndex(currentIndex);
    }

    {
        const QSignalBlocker blocker(languageCombo_);
        int index = languageCombo_->findData(AppSettings::instance().language());
        if (index < 0)
            index = 0;
        languageCombo_->setCurrentIndex(index);
    }
}

void AppearancePage::showEvent(QShowEvent* event)
{
    SettingsPage::showEvent(event);
    load();
}

bool AppearancePage::applyTheme()
{
    const QString themeId = themeCombo_->currentData().toString();
    if (themeId.isEmpty() || themeId == themeManager_->currentThemeId())
        return true;

    const ThemeManagerOperationResult result = themeManager_->selectTheme(themeId);
    if (result.ok())
        return true;

    QStringList details;
    const qsizetype limit = std::min<qsizetype>(result.diagnostics.size(), 3);
    for (qsizetype index = 0; index < limit; ++index)
    {
        const ThemePackageDiagnostic& diagnostic = result.diagnostics.at(index);
        QString location = diagnostic.file;
        if (!diagnostic.path.isEmpty())
            location += QLatin1Char(':') + diagnostic.path;
        details.append(QStringLiteral("%1: %2 (%3)")
                           .arg(themePackageErrorCodeName(diagnostic.code),
                                diagnostic.message, location));
    }
    load();
    showError(
        tr("The selected theme could not be applied. The current theme was kept.\n%1")
            .arg(details.join(QLatin1Char('\n'))));
    return false;
}

void AppearancePage::applyLanguage()
{
    const QString code = languageCombo_->currentData().toString();
    if (code.isEmpty() || code == AppSettings::instance().language())
        return;
    AppSettings::instance().setLanguage(code);
}

}
