
#include "Dialogs/Settings/SettingsPage.h"

#include "Core/QtControlSkin.h"
#include "Core/ContextInfo.h"
#include "Widgets/Common/PanelScrollArea.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/ToastPresenter.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLabel>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace VNX {

namespace {

void appendModelSearchText(const QAbstractItemModel* model, const QModelIndex& parent,
                           QStringList& text)
{
    for (int row = 0; row < model->rowCount(parent); ++row) {
        for (int column = 0; column < model->columnCount(parent); ++column) {
            const auto index = model->index(row, column, parent);
            text.append(index.data(Qt::DisplayRole).toString());
            text.append(index.data(Qt::ToolTipRole).toString());
        }
        appendModelSearchText(model, model->index(row, 0, parent), text);
    }
}

void appendWidgetSearchText(const QWidget* widget, QStringList& text)
{
    if (qobject_cast<const SearchField*>(widget))
        return;

    text.append(widget->toolTip());
    text.append(widget->accessibleName());
    text.append(widget->accessibleDescription());

    if (auto* label = qobject_cast<const QLabel*>(widget))
        text.append(label->text());
    else if (auto* button = qobject_cast<const QAbstractButton*>(widget))
        text.append(button->text());
    else if (auto* combo = qobject_cast<const QComboBox*>(widget)) {
        for (int index = 0; index < combo->count(); ++index) {
            text.append(combo->itemText(index));
            text.append(combo->itemData(index, Qt::ToolTipRole).toString());
        }
        return;
    }
    else if (auto* view = qobject_cast<const QAbstractItemView*>(widget)) {
        if (const auto* model = view->model()) {
            for (int column = 0; column < model->columnCount(); ++column)
                text.append(model->headerData(column, Qt::Horizontal).toString());
            appendModelSearchText(model, view->rootIndex(), text);
        }
        return;
    }
    else if (qobject_cast<const QLineEdit*>(widget))
        return;

    const auto children = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (const auto* child : children)
        appendWidgetSearchText(child, text);
}

}

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
{
}

bool SettingsPage::applySearchFilter(const QString& query)
{
    const auto terms = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const auto matches = [&](QWidget* widget) {
        if (terms.isEmpty())
            return true;
        QStringList text{categoryLabel()};
        appendWidgetSearchText(widget, text);
        const QString searchableText = text.join(QLatin1Char(' '));
        return std::all_of(terms.cbegin(), terms.cend(), [&](const QString& term) {
            return searchableText.contains(term, Qt::CaseInsensitive);
        });
    };

    if (sections_.isEmpty())
        return matches(this);

    bool anyMatch = false;
    for (auto* section : sections_) {
        const bool match = matches(section);
        section->setVisible(match);
        anyMatch = anyMatch || match;
    }
    return anyMatch;
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
    if (!title.isEmpty()) {
        auto* label = new QLabel(title, section);
        QtControlSkin::setSemanticProperty(label, "settingsRole",
                                           QStringLiteral("subtitle"));
        inner->addWidget(label);
    }
    sections_.append(section);
    return section;
}

DialogFormLayout* SettingsPage::makeForm()
{
    return new DialogFormLayout();
}

}
