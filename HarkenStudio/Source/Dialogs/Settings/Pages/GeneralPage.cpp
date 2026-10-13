
#include "Dialogs/Settings/Pages/GeneralPage.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/ThemePackageParser.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>

namespace VNX {

GeneralPage::GeneralPage(ThemeManager& themeManager, QWidget* parent)
    : SettingsPage(parent)
    , themeManager_(&themeManager)
{
    auto* body = makePageBody(tr("General"));

    auto* appearance = makeGroup(tr("Appearance"));
    auto* appearanceForm = makeForm();
    auto* themeLabel = new QLabel(tr("Theme:"));
    QtControlSkin::setSemanticProperty(themeLabel, "settingsRole",
                                       QStringLiteral("formLabel"));
    themeCombo_ = new QComboBox();
    themeCombo_->setObjectName(QStringLiteral("appearanceThemeCombo"));
    const auto themeTopic = ContextTopic::forPanel(QStringLiteral("settings.general.theme"),
        {QDir::toNativeSeparators(themeManager_->userThemesRoot())});
    ContextInfo::tag(themeCombo_, themeTopic);
    ContextInfo::tag(themeLabel, themeTopic);
    QtControlSkin::setSemanticProperty(themeCombo_, "settingsRole",
                                       QStringLiteral("combo"));
    themeLabel->setBuddy(themeCombo_);
    appearanceForm->addRow(themeLabel, themeCombo_);

    auto* languageLabel = new QLabel(tr("Language:"));
    QtControlSkin::setSemanticProperty(languageLabel, "settingsRole",
                                       QStringLiteral("formLabel"));
    languageCombo_ = new QComboBox();
    languageCombo_->setObjectName(QStringLiteral("appearanceLanguageCombo"));
    const auto languageTopic = ContextTopic::forPanel(QStringLiteral("settings.general.language"));
    ContextInfo::tag(languageCombo_, languageTopic);
    ContextInfo::tag(languageLabel, languageTopic);
    QtControlSkin::setSemanticProperty(languageCombo_, "settingsRole",
                                       QStringLiteral("combo"));
    populateLanguageCombo();
    languageLabel->setBuddy(languageCombo_);
    appearanceForm->addRow(languageLabel, languageCombo_);
    static_cast<QVBoxLayout*>(appearance->layout())->addLayout(appearanceForm);

    body->addWidget(appearance);

    auto* developer = makeGroup(tr("Developer"));
    developerModeCheck_ = new QCheckBox(tr("Developer mode"));
    developerModeCheck_->setObjectName(QStringLiteral("developerModeCheck"));
    ContextInfo::tag(developer, ContextTopic::forPanel(QStringLiteral("settings.general.developer")));
    developer->layout()->addWidget(developerModeCheck_);
    body->addWidget(developer);

    body->addStretch();

    connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &GeneralPage::dirtyChanged);
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &GeneralPage::dirtyChanged);
    connect(developerModeCheck_, &QCheckBox::toggled,
            this, &GeneralPage::dirtyChanged);
}

QString GeneralPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageGeneral);
}

QString GeneralPage::categoryLabel() const
{
    return tr("General");
}

void GeneralPage::populateLanguageCombo()
{
    languageCombo_->addItem(tr("English"), QStringLiteral("en"));
    languageCombo_->addItem(tr("Chinese (Simplified)"), QStringLiteral("zh-CN"));
}

void GeneralPage::rememberBaseline()
{
    baselineThemeId_ = themeCombo_->currentData().toString();
    baselineLanguage_ = languageCombo_->currentData().toString();
    baselineDeveloperMode_ = developerModeCheck_->isChecked();
}

void GeneralPage::load()
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

    {
        const QSignalBlocker blocker(developerModeCheck_);
        developerModeCheck_->setChecked(AppSettings::instance().developerMode());
    }

    rememberBaseline();
}

bool GeneralPage::applyTheme()
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
    QMessageBox::warning(
        this, tr("Theme Selection Failed"),
        tr("The selected theme could not be applied. The current theme was kept.\n\n%1")
            .arg(details.join(QLatin1Char('\n'))));
    return false;
}

void GeneralPage::applyLanguage()
{
    const QString code = languageCombo_->currentData().toString();
    if (code.isEmpty() || code == AppSettings::instance().language())
        return;
    AppSettings::instance().setLanguage(code);
}

void GeneralPage::applyDeveloperMode()
{
    AppSettings::instance().setDeveloperMode(developerModeCheck_->isChecked());
}

bool GeneralPage::apply()
{
    revertThemeId_ = themeManager_->currentThemeId();
    revertLanguage_ = AppSettings::instance().language();
    revertDeveloperMode_ = AppSettings::instance().developerMode();
    canRevert_ = false;

    if (!applyTheme())
        return false;
    applyLanguage();
    applyDeveloperMode();
    canRevert_ = true;
    rememberBaseline();
    emit dirtyChanged();
    return true;
}

void GeneralPage::revert()
{
    if (!canRevert_)
        return;

    if (themeManager_->currentThemeId() != revertThemeId_)
        themeManager_->selectTheme(revertThemeId_);
    AppSettings::instance().setLanguage(revertLanguage_);
    AppSettings::instance().setDeveloperMode(revertDeveloperMode_);
    canRevert_ = false;
    emit dirtyChanged();
}

bool GeneralPage::isDirty() const
{
    return themeCombo_->currentData().toString() != baselineThemeId_
        || languageCombo_->currentData().toString() != baselineLanguage_
        || developerModeCheck_->isChecked() != baselineDeveloperMode_;
}

}
