#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/BaseDialog.h"
#include "Core/FontManager.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableWidget.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>

namespace harken::studio {

void AppearancePage::chooseSystemFont(FontRole role)
{
    if (systemFontDialog_ || !basePalette_) return;
    auto* dialog = new BaseDialog(this);
    systemFontDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("appearanceSystemFontDialog"));
    dialog->setWindowTitle(tr("Select System Font"));
    const auto& metrics = themeManager_->metrics().dialog;
    dialog->resize(metrics.compactWidth, metrics.preferredHeight);

    auto* layout = new QVBoxLayout(dialog);
    auto* search = new SearchField(dialog);
    search->setObjectName(QStringLiteral("appearanceSystemFontSearch"));
    search->setPlaceholderText(tr("Search font families..."));
    search->setAccessibleName(tr("Search font families"));
    layout->addWidget(search);
    auto* table = new TableWidget({TableKind::Config, TableBodySizing::Expanding}, dialog, themeManager_);
    auto* list = table->createNavigationView(QStringLiteral("appearanceSystemFontList"));
    list->setAccessibleName(tr("System font families"));
    auto& fonts = FontManager::instance();
    list->addItems(fonts.systemFamilies());
    const auto current = fonts.familyFor(themeManager_->unscaledSnapshot(), role);
    list->setCurrentItem(list->findItems(current.family, Qt::MatchFixedString).value(0, list->item(0)));
    auto* empty = table->bodyMessage();
    empty->setText(tr("No matching font families."));
    layout->addWidget(table, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(list, &QListWidget::itemActivated, dialog, &QDialog::accept);
    layout->addWidget(buttons);

    const auto filter = [list, ok, empty](const QString& text) {
        QListWidgetItem* first = nullptr;
        for (int row = 0; row < list->count(); ++row) {
            auto* item = list->item(row);
            const bool matches = item->text().contains(text, Qt::CaseInsensitive);
            item->setHidden(!matches);
            if (matches && !first) first = item;
        }
        if (!list->currentItem() || list->currentItem()->isHidden())
            list->setCurrentItem(first);
        ok->setEnabled(first != nullptr);
        empty->setVisible(!first);
    };
    connect(search, &QLineEdit::textChanged, dialog, filter);
    filter(search->text());

    const auto themeId = themeManager_->currentThemeId();
    connect(dialog, &QDialog::finished, this, [this, list, themeId, role](int result) {
        systemFontDialog_.clear();
        auto* item = list->currentItem();
        if (result != QDialog::Accepted || !item || item->isHidden()) return;
        auto next = themeManager_->appearance();
        if (next.themeId != themeId) return;
        auto& fields = next.fonts[themeId][QString::fromLatin1(themeFontToken(role).key)];
        fields.family = item->text();
        fields.source = ThemeFontSource::System;
        commitAppearance(next);
    });
    dialog->open();
    if (auto* selected = list->currentItem())
        list->scrollToItem(selected, QAbstractItemView::PositionAtCenter);
    search->setFocus();
}

}
