
#include "Dialogs/Settings/SettingsPage.h"

#include "Core/QtControlSkin.h"
#include "Core/ContextInfo.h"
#include "Widgets/Common/PanelScrollArea.h"
#include "Widgets/Common/ToastPresenter.h"

#include <QLabel>

namespace VNX {

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
{
}

void SettingsPage::showError(const QString& message)
{
    if (!feedback_)
        feedback_ = new ToastPresenter(this);
    feedback_->showMessage(message);
}

QVBoxLayout* SettingsPage::makePageBody()
{
    ContextInfo::tag(this, ContextTopic::forPanel(QStringLiteral("settings.") + pageId()));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new PanelScrollArea(this);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    return layout;
}

QWidget* SettingsPage::makeSection(const QString& title)
{
    auto* section = new QWidget();
    auto* inner = new QVBoxLayout(section);
    inner->setContentsMargins(0, 0, 0, 0);
    inner->setSpacing(DialogFormLayout::RowSpacing);
    auto* label = new QLabel(title, section);
    QtControlSkin::setSemanticProperty(label, "settingsRole",
                                       QStringLiteral("subtitle"));
    inner->addWidget(label);
    return section;
}

DialogFormLayout* SettingsPage::makeForm()
{
    return new DialogFormLayout();
}

}
