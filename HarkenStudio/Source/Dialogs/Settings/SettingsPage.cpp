
#include "Dialogs/Settings/SettingsPage.h"

#include "Core/QtControlSkin.h"
#include "Core/ContextInfo.h"

#include <QLabel>

namespace VNX {

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
{
}

void SettingsPage::revert()
{
}

QVBoxLayout* SettingsPage::makePageBody(const QString& title)
{
    return makePageBody(this, title);
}

QVBoxLayout* SettingsPage::makePageBody(QWidget* host, const QString& title)
{
    ContextInfo::tag(this, ContextTopic::forPanel(QStringLiteral("settings.") + pageId()));
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* titleLabel = new QLabel(title, host);
    QtControlSkin::setSemanticProperty(titleLabel, "settingsRole",
                                       QStringLiteral("title"));
    layout->addWidget(titleLabel);

    return layout;
}

QGroupBox* SettingsPage::makeGroup(const QString& title)
{
    auto* box = new QGroupBox(title);
    QtControlSkin::setSemanticProperty(box, "settingsRole",
                                       QStringLiteral("group"));
    auto* inner = new QVBoxLayout(box);
    inner->setContentsMargins(16, 20, 16, 16);
    inner->setSpacing(12);
    return box;
}

QFormLayout* SettingsPage::makeForm()
{
    auto* form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return form;
}

}
