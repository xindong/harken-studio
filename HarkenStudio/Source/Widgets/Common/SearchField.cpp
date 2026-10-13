
#include "SearchField.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/QtControlSkin.h"
#include "../../Core/ThemeAssetCatalog.h"

#include <QAction>

namespace VNX {

SearchField::SearchField(QWidget* parent)
    : QLineEdit(parent)
{
    setPlaceholderText(tr("Search..."));
    searchAction_ = addAction(QIcon(), QLineEdit::LeadingPosition);
    clearAction_ = addAction(QIcon(), QLineEdit::TrailingPosition);
    clearAction_->setVisible(false);
    connect(clearAction_, &QAction::triggered, this, &QLineEdit::clear);
    connect(this, &QLineEdit::textChanged, this, [this](const QString& text) {
        clearAction_->setVisible(!text.isEmpty());
    });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });
}

void SearchField::applyTheme()
{
    QtControlSkin::setSemanticProperty(this, "fieldRole",
                                      QStringLiteral("search"));
    searchAction_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::Search)).icon);
    clearAction_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::SearchClear)).icon);
}

}
