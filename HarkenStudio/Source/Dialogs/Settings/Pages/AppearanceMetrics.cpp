#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/ThemeLayout.h"
#include "Core/ThemeStyleTokens.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"

#include <QHash>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidgetItemIterator>

namespace harken::studio {

void AppearancePage::buildMetrics(QWidget* section)
{
    auto* layout = static_cast<QVBoxLayout*>(section->layout());
    auto* table = new TableWidget(TableOptions::display(), section, themeManager_);
    table->setColumnsMovable(false);
    themeManager_->bindMetrics(table, [table](const ThemeMetrics& metrics) {
        table->setFixedHeight(metrics.panel.previewHeight);
    });
    metricTree_ = table->createItemView(QStringLiteral("appearanceMetricTree"),
        {tr("Dimension"), tr("Value"), tr("Theme Default")}, {1, 1, 1});
    metricTree_->setAccessibleName(tr("Theme dimensions"));
    metricTree_->setRootIsDecorated(false);
    metricTree_->setAllColumnsShowFocus(true);
    metricTree_->setContextMenuPolicy(Qt::PreventContextMenu);
    QHash<QString, QTreeWidgetItem*> groups;
    for (const auto& token : themeMetricTokens()) {
        const QString key = QString::fromLatin1(token.key);
        auto*& group = groups[QString::fromLatin1(token.group)];
        if (!group)
            group = metricTree_->createGroup(tr(token.group));
        auto* row = new QTreeWidgetItem(group, {tr(token.label)});
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        row->setData(0, Qt::UserRole, key);
        const QString description = tr(token.description);
        for (int column = 0; column < metricTree_->columnCount(); ++column)
            row->setToolTip(column, description);

        auto* value = new QSpinBox(metricTree_);
        value->setObjectName(QStringLiteral("appearanceMetricValue"));
        value->setAccessibleName(tr("%1 %2").arg(row->text(0), group->text(0).toLower()));
        value->setSuffix(tr(" px"));
        value->setRange(token.minimum, ThemeMetricToken::maximum);
        value->setKeyboardTracking(false);
        value->setToolTip(description);
        value->setContextMenuPolicy(Qt::PreventContextMenu);
        value->findChild<QLineEdit*>()->setContextMenuPolicy(Qt::PreventContextMenu);
        metricTree_->setItemWidget(row, 1, value);
        connect(value, &QSpinBox::valueChanged, this, [this, key](int number) {
            if (!basePalette_) return;
            auto next = themeManager_->appearance();
            if (number == findThemeMetricToken(key)->value(basePalette_->snapshot->ui.metrics))
                next.metrics[next.themeId].remove(key);
            else
                next.metrics[next.themeId].insert(key, number);
            commitAppearance(next);
        });
    }
    layout->addWidget(table);

    auto* actions = new QHBoxLayout();
    ThemeLayout::spacing(actions, ThemeLayout::Space::Tight, *themeManager_);
    metricsResetSelected_ = new QPushButton(tr("Reset Selected"), section);
    metricsResetSelected_->setObjectName(QStringLiteral("appearanceMetricsResetSelected"));
    metricsResetSelected_->setToolTip(tr("Restore the selected dimension to its theme default."));
    metricsReset_ = new QPushButton(tr("Reset All"), section);
    metricsReset_->setObjectName(QStringLiteral("appearanceMetricsReset"));
    metricsReset_->setToolTip(tr("Restore this theme's default dimensions."));
    actions->addStretch();
    actions->addWidget(metricsResetSelected_);
    actions->addWidget(metricsReset_);
    layout->addLayout(actions);

    connect(metricTree_, &QTreeWidget::itemSelectionChanged, this, &AppearancePage::refreshMetricActions);
    connect(metricTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* group) {
        const auto selected = metricTree_->selectedItems();
        if (!selected.isEmpty() && selected.front()->parent() == group)
            metricTree_->clearSelection();
    });
    connect(metricsResetSelected_, &QPushButton::clicked, this, [this] {
        const auto selected = metricTree_->selectedItems();
        if (selected.isEmpty()) return;
        auto next = themeManager_->appearance();
        next.metrics[next.themeId].remove(selected.front()->data(0, Qt::UserRole).toString());
        commitAppearance(next);
    });
    connect(metricsReset_, &QPushButton::clicked, this, [this] {
        auto next = themeManager_->appearance();
        next.metrics.remove(next.themeId);
        commitAppearance(next);
    });
}

void AppearancePage::refreshMetrics()
{
    const auto& metrics = themeManager_->unscaledSnapshot()->ui.metrics;
    const auto overrides = appearance_.metrics.value(appearance_.themeId);
    metricTree_->setEnabled(basePalette_.has_value());
    for (QTreeWidgetItemIterator it(metricTree_, QTreeWidgetItemIterator::NoChildren); *it; ++it) {
        auto* row = *it;
        const QString key = row->data(0, Qt::UserRole).toString();
        const auto* token = findThemeMetricToken(key);
        auto* value = static_cast<QSpinBox*>(metricTree_->itemWidget(row, 1));
        const QSignalBlocker blocker(value);
        value->setValue(token->value(metrics));
        row->setText(2, basePalette_ ? tr("%1 px").arg(token->value(basePalette_->snapshot->ui.metrics)) : QString());
        row->setData(0, TableTextRole, static_cast<int>(overrides.contains(key)
            ? TableTextState::Custom : TableTextState::Normal));
    }
    refreshMetricActions();
}

void AppearancePage::refreshMetricActions()
{
    const auto selected = metricTree_->selectedItems();
    const auto overrides = appearance_.metrics.value(appearance_.themeId);
    metricsResetSelected_->setEnabled(basePalette_ && !selected.isEmpty()
        && overrides.contains(selected.front()->data(0, Qt::UserRole).toString()));
    metricsReset_->setEnabled(basePalette_ && !overrides.isEmpty());
}

}
