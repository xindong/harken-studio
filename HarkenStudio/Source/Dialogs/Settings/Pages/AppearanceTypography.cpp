#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/FontManager.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeStyleTokens.h"
#include "Widgets/Common/MenuComboBox.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"

#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

namespace harken::studio {

void AppearancePage::buildTypography(QWidget* section)
{
    auto* layout = static_cast<QVBoxLayout*>(section->layout());
    typographyTable_ = new TableWidget(TableOptions::display(TableBodySizing::Content), section, themeManager_);
    typographyTable_->setColumnsMovable(false);
    fontTree_ = typographyTable_->createItemView(QStringLiteral("appearanceFontTree"),
        {tr("Role"), tr("Family"), tr("Size"), tr("Weight")}, {1, 1, 1, 1});
    fontTree_->setAccessibleName(tr("Typography"));
    fontTree_->setAccessibleDescription(tr("Font family, size, and weight for each text role."));
    fontTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    fontTree_->setContextMenuPolicy(Qt::PreventContextMenu);
    fontTree_->setAllColumnsShowFocus(true);
    layout->addWidget(typographyTable_);
    for (const auto& token : themeFontTokens()) {
        const QString key = QString::fromLatin1(token.key);
        const QString name = tr(token.label);
        auto* item = new QTreeWidgetItem(fontTree_, {name});
        item->setData(0, Qt::UserRole, key);
        item->setToolTip(0, tr(token.description));

        auto* family = new MenuComboBox(fontTree_);
        family->setObjectName(QStringLiteral("appearanceFontFamily_") + key);
        family->setAccessibleName(tr("%1 font family").arg(name));
        family->setContextMenuPolicy(Qt::PreventContextMenu);
        family->setFocusPolicy(Qt::StrongFocus);
        family->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        fontTree_->setItemWidget(item, 1, family);
        auto* size = new QSpinBox(fontTree_);
        size->setObjectName(QStringLiteral("appearanceFontSize_") + key);
        size->setAccessibleName(tr("%1 font size").arg(name));
        size->setRange(ThemeFontOverride::minimumPointSize, ThemeFontOverride::maximumPointSize);
        size->setKeyboardTracking(false);
        size->setSuffix(tr(" pt"));
        fontTree_->setItemWidget(item, 2, size);
        auto* weight = new QSpinBox(fontTree_);
        weight->setObjectName(QStringLiteral("appearanceFontWeight_") + key);
        weight->setAccessibleName(tr("%1 font weight").arg(name));
        weight->setRange(ThemeFontOverride::minimumWeight, ThemeFontOverride::maximumWeight);
        weight->setKeyboardTracking(false);
        fontTree_->setItemWidget(item, 3, weight);
        for (auto* field : {size, weight}) {
            field->setContextMenuPolicy(Qt::PreventContextMenu);
            field->findChild<QLineEdit*>()->setContextMenuPolicy(Qt::PreventContextMenu);
        }
        if (token.role == FontRole::Title)
            weight->setToolTip(tr("Title weight is also used for emphasized text throughout the editor."));
        fontRows_.insert(key, {family, size, weight});

        const auto edit = [this, key, member = token.member](auto update) {
            if (!basePalette_) return;
            auto next = themeManager_->appearance();
            auto& fields = next.fonts[next.themeId][key];
            update(fields, basePalette_->snapshot->ui.typography.*member);
            if (fields.empty()) next.fonts[next.themeId].remove(key);
            if (next.fonts.value(next.themeId).isEmpty()) next.fonts.remove(next.themeId);
            commitAppearance(next);
        };
        auto* menu = family->menu();
        connect(menu, &QMenu::aboutToShow, this, [this, item, menu, key, role = token.role] {
            fontTree_->setCurrentItem(item);
            menu->clear();
            auto& fonts = FontManager::instance();
            const auto snapshot = themeManager_->unscaledSnapshot();
            const auto effective = fonts.familyFor(snapshot, role);
            const auto fields = appearance_.fonts.value(appearance_.themeId).value(key);
            const auto currentName = fields.family.value_or(effective.family);
            const auto currentSource = fields.family ? fields.source.value_or(ThemeFontSource::System) : effective.source;
            for (const auto& name : fonts.themeFamilies(snapshot)) {
                auto* action = menu->addAction(QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")));
                action->setData(name);
                action->setCheckable(true);
                action->setChecked(currentSource == ThemeFontSource::Theme && name.compare(currentName, Qt::CaseInsensitive) == 0);
            }
            if (!menu->isEmpty()) menu->addSeparator();
            menu->addAction(tr("Use Font in Current System..."));
        });
        connect(menu, &QMenu::triggered, this, [this, role = token.role, edit](QAction* action) {
            if (!action->data().isValid()) {
                chooseSystemFont(role);
                return;
            }
            edit([action](ThemeFontOverride& fields, const ThemeFontStyle&) {
                fields.family = action->data().toString();
                fields.source = ThemeFontSource::Theme;
            });
        });
        connect(themeManager_, &ThemeManager::appearanceChanged, menu, &QMenu::close);
        connect(size, &QSpinBox::valueChanged, this, [edit](int value) {
            edit([value](ThemeFontOverride& fields, const ThemeFontStyle& original) {
                fields.pointSize = value == original.pointSize ? std::nullopt : std::optional{value};
            });
        });
        connect(weight, &QSpinBox::valueChanged, this, [edit](int value) {
            edit([value](ThemeFontOverride& fields, const ThemeFontStyle& original) {
                fields.weight = value == original.weight ? std::nullopt : std::optional{value};
            });
        });
    }
    auto* actions = new QHBoxLayout();
    ThemeLayout::spacing(actions, ThemeLayout::Space::Tight, *themeManager_);
    actions->addStretch();
    fontRoleReset_ = new QPushButton(tr("Reset Selected"), section);
    fontRoleReset_->setObjectName(QStringLiteral("appearanceFontRoleReset"));
    actions->addWidget(fontRoleReset_);
    typographyReset_ = new QPushButton(tr("Reset All"), section);
    typographyReset_->setObjectName(QStringLiteral("appearanceTypographyReset"));
    actions->addWidget(typographyReset_);
    layout->addLayout(actions);
    connect(fontTree_, &QTreeWidget::itemSelectionChanged, this, &AppearancePage::refreshTypographyReset);
    connect(fontRoleReset_, &QPushButton::clicked, this, [this] {
        const auto* item = fontTree_->selectedItems().value(0);
        if (!item) return;
        auto next = themeManager_->appearance();
        next.fonts[next.themeId].remove(item->data(0, Qt::UserRole).toString());
        if (next.fonts.value(next.themeId).isEmpty()) next.fonts.remove(next.themeId);
        commitAppearance(next);
    });
    connect(typographyReset_, &QPushButton::clicked, this, [this] {
        auto next = themeManager_->appearance();
        next.fonts.remove(next.themeId);
        commitAppearance(next);
    });
}

void AppearancePage::refreshTypographyReset()
{
    const auto* item = fontTree_->selectedItems().value(0);
    const auto overrides = appearance_.fonts.value(appearance_.themeId);
    fontRoleReset_->setEnabled(basePalette_ && item && overrides.contains(item->data(0, Qt::UserRole).toString()));
    typographyReset_->setEnabled(basePalette_ && !overrides.isEmpty());
}

void AppearancePage::refreshTypography()
{
    typographyTable_->setEnabled(basePalette_.has_value());
    refreshTypographyReset();
    if (!basePalette_) return;
    const auto unscaled = themeManager_->unscaledSnapshot();
    auto& fonts = FontManager::instance();
    const auto overrides = appearance_.fonts.value(appearance_.themeId);
    for (const auto& token : themeFontTokens()) {
        const QString key = QString::fromLatin1(token.key);
        const auto& row = fontRows_[key];
        const auto fields = overrides.value(key);
        const QFont current = fonts.getFont(unscaled, token.role);
        const auto effective = fonts.familyFor(unscaled, token.role);
        const auto formatFamily = [](const QString& name, ThemeFontSource source) {
            return source == ThemeFontSource::Theme ? tr("%1 (From Theme)").arg(name) : tr("%1 (From System)").arg(name);
        };
        const QString activeFamily = formatFamily(effective.family, effective.source);
        const auto chosenSource = fields.source.value_or(ThemeFontSource::System);
        const bool unavailable = fields.family && (chosenSource != effective.source
            || fields.family->compare(effective.family, Qt::CaseInsensitive) != 0);
        row.family->setDisplayText(unavailable ? tr("%1 (Unavailable)").arg(*fields.family) : effective.family);
        row.family->setToolTip(unavailable ? tr("%1 is unavailable. Using %2.")
            .arg(formatFamily(*fields.family, chosenSource), activeFamily) : activeFamily);
        const QSignalBlocker sizeBlocker(row.size);
        const QSignalBlocker weightBlocker(row.weight);
        row.size->setValue(current.pointSize());
        row.weight->setValue(int(current.weight()));
    }
}

}
