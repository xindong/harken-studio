#include "Dialogs/Settings/Pages/ExtensionPage.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"

#include <QCheckBox>
#include <QSignalBlocker>

namespace VNX {

ExtensionPage::ExtensionPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();
    auto* developer = makeSection(tr("Developer"));
    auto* developerForm = makeForm();
    developerModeCheck_ = new QCheckBox();
    developerModeCheck_->setObjectName(QStringLiteral("developerModeCheck"));
    ContextInfo::tag(developer, ContextTopic::forPanel(QStringLiteral("settings.extension.developer")));
    developerForm->addRow(tr("Developer mode"), developerModeCheck_);
    static_cast<QVBoxLayout*>(developer->layout())->addLayout(developerForm);
    body->addWidget(developer);
    body->addStretch();

    connect(developerModeCheck_, &QCheckBox::toggled,
            this, &ExtensionPage::dirtyChanged);
}

QString ExtensionPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageExtension);
}

QString ExtensionPage::categoryLabel() const
{
    return tr("Extension");
}

void ExtensionPage::load()
{
    const QSignalBlocker blocker(developerModeCheck_);
    developerModeCheck_->setChecked(AppSettings::instance().developerMode());
    baselineDeveloperMode_ = developerModeCheck_->isChecked();
}

bool ExtensionPage::apply()
{
    revertDeveloperMode_ = AppSettings::instance().developerMode();
    AppSettings::instance().setDeveloperMode(developerModeCheck_->isChecked());
    canRevert_ = true;
    baselineDeveloperMode_ = developerModeCheck_->isChecked();
    emit dirtyChanged();
    return true;
}

void ExtensionPage::revert()
{
    if (!canRevert_)
        return;

    AppSettings::instance().setDeveloperMode(revertDeveloperMode_);
    canRevert_ = false;
    emit dirtyChanged();
}

bool ExtensionPage::isDirty() const
{
    return developerModeCheck_->isChecked() != baselineDeveloperMode_;
}

}
