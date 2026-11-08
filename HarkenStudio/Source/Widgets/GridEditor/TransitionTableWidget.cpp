#include "TransitionTableWidget.h"
#include "TransitionFadeButton.h"

#include "Core/ActionManager.h"
#include "Core/ActionStepUtils.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VSelection.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/TransitionFieldGating.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QStandardItemModel>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

namespace {

ProjectItem* findItem(const std::optional<QUuid>& guid)
{
    return guid ? ProjectManager::instance().findItemByGuid(*guid) : nullptr;
}

TimelineItemResolver itemResolver()
{
    return [](const QUuid& guid) { return ProjectManager::instance().findItemByGuid(guid); };
}

QString exitTimingText(TransitionExitTiming timing)
{
    switch (timing) {
        case TransitionExitTiming::Immediate:  return TransitionTableWidget::tr("Immediate");
        case TransitionExitTiming::NaturalEnd: return TransitionTableWidget::tr("Natural End");
        case TransitionExitTiming::NextBeat:   return TransitionTableWidget::tr("Next Beat");
        case TransitionExitTiming::NextBar:    return TransitionTableWidget::tr("Next Bar");
        case TransitionExitTiming::NextMarker: return TransitionTableWidget::tr("Next Marker");
        case TransitionExitTiming::ExitMarker: return TransitionTableWidget::tr("Exit Marker");
    }
    return QString();
}

QString entryModeText(TransitionEntryMode mode)
{
    switch (mode) {
        case TransitionEntryMode::EntryMarker:  return TransitionTableWidget::tr("Entry Marker");
        case TransitionEntryMode::SamePosition: return TransitionTableWidget::tr("Same Position");
    }
    return QString();
}

FieldAvail exitAvailability(const ExitTimingAvail& exit, TransitionExitTiming timing, QString* reason)
{
    switch (timing) {
        case TransitionExitTiming::Immediate:  return exit.immediate;
        case TransitionExitTiming::NaturalEnd: return exit.naturalEnd;
        case TransitionExitTiming::NextBeat:   *reason = exit.reasonNextBeat; return exit.nextBeat;
        case TransitionExitTiming::NextBar:    *reason = exit.reasonNextBar; return exit.nextBar;
        case TransitionExitTiming::NextMarker: *reason = exit.reasonNextMarker; return exit.nextMarker;
        case TransitionExitTiming::ExitMarker: *reason = exit.reasonExitMarker; return exit.exitMarker;
    }
    return FieldAvail::Available;
}

}

TransitionTableWidget::TransitionTableWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("transitionTable"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    table_ = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, this);
    table_->setAccessibleName(tr("Transitions"));
    table_->setColumns(QStringLiteral("Transition"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0},
        {QStringLiteral("Source"), tr("Source"), 230},
        {QStringLiteral("Exit"), tr("Exit"), 150},
        {QStringLiteral("FadeOut"), tr("Fade Out"), 120},
        {QStringLiteral("Target"), tr("Target"), 230},
        {QStringLiteral("Entry"), tr("Entry"), 150},
        {QStringLiteral("FadeIn"), tr("Fade In"), 120}
    });
    table_->headerCell(0)->setToolTip(tr("Match order. More specific rules win; the default rule is always last."));
    table_->setSelectionClearHandler([this] { selectRow(-1); });
    table_->setRowSelectionHandler([this](QWidget* row) {
        selectRow(row->property("rowIndex").toInt());
    });
    root->addWidget(table_, 1);

    auto* footer = table_->footer();
    addButton_ = footer->addButton();
    addButton_->setObjectName(QStringLiteral("transitionAddRule"));
    removeButton_ = footer->removeButton();
    removeButton_->setObjectName(QStringLiteral("transitionRemoveRule"));
    connect(addButton_, &QToolButton::clicked, this, [this] { addRule(); });
    connect(removeButton_, &QToolButton::clicked, this, [this] { removeSelected(); });

    auto& manager = ProjectManager::instance();
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) { rebuildRows(); });
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (selfWriting_ || !boundItem_)
            return;
        if ((item == boundItem_ && property == QStringLiteral("transitions"))
            || property == QStringLiteral("segmentTime")
            || property == QStringLiteral("markers")
            || property == QStringLiteral("clips"))
            rebuildRows();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemRenamed, this, [this](ProjectItem* item, const QString&) {
        if (boundItem_ && item && item->parent == boundItem_)
            rebuildRows();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemCreated, this, [this](ProjectItem* item) {
        if (boundItem_ && item && item->parent == boundItem_)
            refreshFooter();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemMoved, this,
            [this](ProjectItem*, ProjectItem* oldParent, ProjectItem* newParent) {
        if (boundItem_ && (oldParent == boundItem_ || newParent == boundItem_))
            rebuildRows();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item == boundItem_)
            setBoundItem(nullptr);
        else if (boundItem_ && item && item->parent == boundItem_)
            rebuildRows();
    });
}

void TransitionTableWidget::setBoundItem(ProjectItem* item)
{
    if (boundItem_ != item) {
        selected_.reset();
        placement_.reset();
    }
    boundItem_ = item;
    rebuildRows();
}

void TransitionTableWidget::setPlacementContext(const std::optional<PlacementContext>& placement)
{
    placement_ = placement;
    rebuildRows();
}

const TransitionTableDef* TransitionTableWidget::table() const
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    return selection ? &selection->transitions : nullptr;
}

void TransitionTableWidget::rebuildRows()
{
    ++generation_;
    auto* layout = table_->rowsLayout();
    while (QLayoutItem* entry = layout->takeAt(0)) {
        if (QWidget* widget = entry->widget()) {
            for (auto* reference : widget->findChildren<ObjectRefWidget*>())
                reference->cancelPendingSelection();
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete entry;
    }
    rows_.clear();
    rowEndpoints_.clear();

    if (const TransitionTableDef* rules = table()) {
        const std::vector<int> order = TransitionSemantics::rulesForDisplay(*rules);
        for (size_t display = 0; display < order.size(); ++display) {
            const TransitionRuleDef& rule = rules->rules[static_cast<size_t>(order[display])];
            layout->addWidget(buildRow(static_cast<int>(display), rule));
            rowEndpoints_.push_back({rule.source, rule.target});
        }
    }
    table_->refreshLayout();

    int selectedIndex = -1;
    for (size_t index = 0; selected_ && index < rowEndpoints_.size(); ++index)
        if (rowEndpoints_[index] == *selected_)
            selectedIndex = static_cast<int>(index);
    selectRow(selectedIndex);
}

QWidget* TransitionTableWidget::buildRow(int displayIndex, const TransitionRuleDef& rule)
{
    auto* row = new QWidget(table_->rowsWidget());
    row->setObjectName(QStringLiteral("transitionRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setProperty("listRow", true);
    row->setProperty("selected", false);
    row->setProperty("rowIndex", displayIndex);
    DebugTint::tint(row, DebugTint::Role::Row);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* number = new QLabel(QString::number(displayIndex + 1), row);
    number->setObjectName(QStringLiteral("transitionRuleNumber"));
    number->setAlignment(Qt::AlignCenter);
    QtControlSkin::setSemanticProperty(number, "textRole", QStringLiteral("muted"));

    const QList<QWidget*> cells{
        number,
        buildEndpointCell(row, rule, true),
        buildExitCell(row, rule),
        buildFadeCell(row, rule, FadeSlot::SourceOut),
        buildEndpointCell(row, rule, false),
        buildEntryCell(row, rule),
        buildFadeCell(row, rule, FadeSlot::TargetIn)};
    for (QWidget* cell : cells)
        layout->addWidget(cell);
    table_->registerRow(row, cells);

    row->installEventFilter(this);
    for (QWidget* child : row->findChildren<QWidget*>())
        child->installEventFilter(this);
    rows_.push_back(row);
    return row;
}

QWidget* TransitionTableWidget::buildEndpointCell(QWidget* row, const TransitionRuleDef& rule, bool source)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (rule.isDefault()) {
        auto* any = new QLabel(QStringLiteral("*"), cell);
        any->setToolTip(TransitionSemantics::defaultRuleEndpointReason());
        QtControlSkin::setSemanticProperty(any, "textRole", QStringLiteral("muted"));
        cell->addWidget(any);
        return cell;
    }

    const std::optional<QUuid>& endpoint = source ? rule.source : rule.target;
    const Endpoints endpoints{rule.source, rule.target};
    auto* reference = new ObjectRefWidget(cell);
    reference->setObjectName(source ? QStringLiteral("transitionSourceRef")
                                    : QStringLiteral("transitionTargetRef"));
    reference->setCompact(true);
    reference->setCompactPickable(true);
    reference->setRequired(false);
    reference->setClearSupport(true);
    reference->setEmptyText(QStringLiteral("*"));
    reference->setSupplementalToolTip(endpoint ? QString() : tr("* (Any)"));
    reference->setSelectorTitle(source ? tr("Select Source") : tr("Select Target"));
    reference->setFilterTypes(EditorUtils::userFacingActionStepTargetTypes(VActionStep::Type::Play));
    reference->setDirectChildParentGuid(boundItem_->guid);
    reference->setEditGuard([this, generation = generation_] { return boundItem_ && generation == generation_; });
    reference->setIsSelectable([this](uint32_t id) {
        const ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        return boundItem_ && item && ItemTypeCatalog::isUserFacingType(item->type)
            && TransitionSemantics::isEndpointCandidate(*boundItem_, *item);
    });
    if (endpoint && !findItem(endpoint))
        reference->setUnresolvedReference(endpoint->toString(QUuid::WithoutBraces));
    else
        reference->setCurrentObjectID(endpoint ? findItem(endpoint)->objectID : 0);
    connect(reference, &ObjectRefWidget::objectChanged, this,
            [this, endpoints, source, generation = generation_](uint32_t id) {
        if (generation != generation_)
            return;
        const ProjectItem* item = id ? ProjectManager::instance().findItemByObjectID(id) : nullptr;
        const std::optional<QUuid> guid = item ? std::make_optional(item->guid) : std::nullopt;
        if (updateRule(endpoints, [&](TransitionRuleDef& rule) {
                (source ? rule.source : rule.target) = guid;
            }, tr("Set Transition Endpoints")))
            selected_ = source ? Endpoints{guid, endpoints.second} : Endpoints{endpoints.first, guid};
        rebuildRows();
    });
    cell->addWidget(reference);
    return cell;
}

QWidget* TransitionTableWidget::buildExitCell(QWidget* row, const TransitionRuleDef& rule)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* combo = new QComboBox(cell);
    combo->setObjectName(QStringLiteral("transitionExitTiming"));
    const TransitionFieldGating gating = TransitionSemantics::gatingFor(
        *boundItem_, rule, TransitionSemantics::watchAvailabilityFor(
            *boundItem_, rule.source, placement_ ? &*placement_ : nullptr, itemResolver()));
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    for (TransitionExitTiming timing : AuthoringPolicy::allowedTransitionExitTimings()) {
        combo->addItem(exitTimingText(timing), static_cast<int>(timing));
        QString reason;
        const bool enabled = exitAvailability(gating.exit, timing, &reason) != FieldAvail::Unavailable;
        if (auto* item = model ? model->item(combo->count() - 1) : nullptr) {
            item->setEnabled(enabled || timing == rule.exitTiming);
            item->setData(reason.isEmpty() ? QVariant() : QVariant(reason), Qt::ToolTipRole);
        }
    }
    combo->setCurrentIndex(combo->findData(static_cast<int>(rule.exitTiming)));
    combo->setToolTip(gating.exit.warning);
    QtControlSkin::setSemanticProperty(combo, "inputInvalid", !gating.exit.warning.isEmpty());
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, combo, endpoints = Endpoints{rule.source, rule.target}, generation = generation_] {
        if (generation != generation_)
            return;
        const auto timing = static_cast<TransitionExitTiming>(combo->currentData().toInt());
        updateRule(endpoints, [timing](TransitionRuleDef& rule) { rule.exitTiming = timing; },
                   tr("Set Transition Exit Timing"));
        rebuildRows();
    });
    cell->addWidget(combo);
    return cell;
}

QWidget* TransitionTableWidget::buildEntryCell(QWidget* row, const TransitionRuleDef& rule)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* combo = new QComboBox(cell);
    combo->setObjectName(QStringLiteral("transitionEntryMode"));
    for (TransitionEntryMode mode : {TransitionEntryMode::EntryMarker, TransitionEntryMode::SamePosition})
        combo->addItem(entryModeText(mode), static_cast<int>(mode));
    combo->setCurrentIndex(combo->findData(static_cast<int>(rule.entryMode)));
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, combo, endpoints = Endpoints{rule.source, rule.target}, generation = generation_] {
        if (generation != generation_)
            return;
        const auto mode = static_cast<TransitionEntryMode>(combo->currentData().toInt());
        updateRule(endpoints, [mode](TransitionRuleDef& rule) { rule.entryMode = mode; },
                   tr("Set Transition Entry Mode"));
        rebuildRows();
    });
    cell->addWidget(combo);
    return cell;
}

QWidget* TransitionTableWidget::buildFadeCell(QWidget* row, const TransitionRuleDef& rule, FadeSlot slot)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* button = new TransitionFadeButton(cell);
    button->setObjectName(slot == FadeSlot::SourceOut ? QStringLiteral("transitionFadeSourceOut")
                                                      : QStringLiteral("transitionFadeTargetIn"));
    button->setFade(slot == FadeSlot::SourceOut ? rule.sourceFadeOut : rule.targetFadeIn);
    connect(button, &QToolButton::clicked, this,
            [this, button, slot, endpoints = Endpoints{rule.source, rule.target}] {
        openFadePopup(endpoints, slot, button);
    });
    cell->addWidget(button);
    return cell;
}

void TransitionTableWidget::selectRow(int displayIndex)
{
    selected_ = displayIndex >= 0 && displayIndex < static_cast<int>(rowEndpoints_.size())
        ? std::make_optional(rowEndpoints_[static_cast<size_t>(displayIndex)]) : std::nullopt;
    for (size_t index = 0; index < rows_.size(); ++index) {
        QWidget* row = rows_[index];
        row->setProperty("selected", static_cast<int>(index) == displayIndex);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    refreshFooter();
}

void TransitionTableWidget::refreshFooter()
{
    const TransitionTableDef* rules = table();
    if (!boundItem_ || !rules) {
        addButton_->setEnabled(false);
        removeButton_->setEnabled(false);
        return;
    }
    const TransitionRuleDef* selectedRule = nullptr;
    for (const TransitionRuleDef& rule : rules->rules)
        if (selected_ && rule.source == selected_->first && rule.target == selected_->second)
            selectedRule = &rule;
    const TransitionLedgerGating ledger = TransitionSemantics::ledgerGatingFor(
        *boundItem_, *rules, selectedRule, itemResolver());
    const bool canAdd = ledger.addRule == FieldAvail::Available;
    addButton_->setEnabled(canAdd);
    addButton_->setToolTip(canAdd ? tr("Add Rule") : ledger.reasonAddRule);
    const bool canRemove = ledger.removeRule == FieldAvail::Available;
    removeButton_->setEnabled(canRemove);
    removeButton_->setToolTip(canRemove ? tr("Remove Rule") : ledger.reasonRemoveRule);
}

void TransitionTableWidget::addRule()
{
    const TransitionTableDef* rules = table();
    if (!boundItem_ || !rules)
        return;
    const auto combo = TransitionSemantics::firstUnusedCombination(*boundItem_, *rules, itemResolver());
    if (!combo)
        return;
    TransitionRuleDef rule = AuthoringPolicy::defaultTransitionRule();
    rule.source = combo->first;
    rule.target = combo->second;
    TransitionTableDef candidate = *rules;
    if (!candidate.addRule(rule))
        return;
    if (commit(candidate, tr("Add Transition Rule")))
        selected_ = Endpoints{rule.source, rule.target};
    rebuildRows();
}

void TransitionTableWidget::removeSelected()
{
    const TransitionTableDef* rules = table();
    if (!rules || !selected_)
        return;
    for (size_t index = 0; index < rules->rules.size(); ++index) {
        const TransitionRuleDef& rule = rules->rules[index];
        if (rule.isDefault() || rule.source != selected_->first || rule.target != selected_->second)
            continue;
        TransitionTableDef candidate = *rules;
        if (!candidate.removeRule(static_cast<int>(index)))
            return;
        table_->removeSelectedRow([this, candidate] {
            const bool removed = commit(candidate, tr("Remove Transition Rule"));
            rebuildRows();
            return removed;
        });
        return;
    }
}

bool TransitionTableWidget::updateRule(const Endpoints& endpoints,
                                       const std::function<void(TransitionRuleDef&)>& edit,
                                       const QString& undoText)
{
    const TransitionTableDef* rules = table();
    if (!rules)
        return false;
    for (size_t index = 0; index < rules->rules.size(); ++index) {
        TransitionRuleDef rule = rules->rules[index];
        if (rule.source != endpoints.first || rule.target != endpoints.second)
            continue;
        edit(rule);
        TransitionTableDef candidate = *rules;
        return candidate.updateRule(static_cast<int>(index), rule) && commit(candidate, undoText);
    }
    return false;
}

bool TransitionTableWidget::commit(const TransitionTableDef& candidate, const QString& undoText)
{
    if (!boundItem_)
        return false;
    selfWriting_ = true;
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
            {QStringLiteral("property"), QStringLiteral("transitions")},
            {QStringLiteral("value"), candidate.toJson()},
            {QStringLiteral("undo_text"), undoText}},
        ActionSource::GUI, this);
    selfWriting_ = false;
    return result.success;
}

void TransitionTableWidget::openFadePopup(const Endpoints& endpoints, FadeSlot slot, QWidget* anchor)
{
    const auto currentFade = [this, endpoints, slot]() -> TransitionFadeDef {
        if (const TransitionTableDef* rules = table())
            for (const TransitionRuleDef& rule : rules->rules)
                if (rule.source == endpoints.first && rule.target == endpoints.second)
                    return slot == FadeSlot::SourceOut ? rule.sourceFadeOut : rule.targetFadeIn;
        return {};
    };
    const std::optional<QUuid>& owner = slot == FadeSlot::SourceOut ? endpoints.first : endpoints.second;
    TransitionFadePopupContext context;
    if (const ProjectItem* item = findItem(owner)) {
        context.objectName = item->name;
        context.objectIcon = iconForItem(*item);
    } else {
        context.objectName = tr("* (Any)");
        context.dimmed = true;
    }
    auto* popup = new TransitionFadePopup(
        slot == FadeSlot::SourceOut ? tr("Fade Out") : tr("Fade In"), context, currentFade(), this,
        [this, endpoints, slot](const TransitionFadeDef& fade) {
            updateRule(endpoints, [&](TransitionRuleDef& rule) {
                (slot == FadeSlot::SourceOut ? rule.sourceFadeOut : rule.targetFadeIn) = fade;
            }, tr("Set Transition Fade"));
            rebuildRows();
        },
        currentFade);
    popup->showAt(anchor->mapToGlobal(anchor->rect().bottomLeft()));
}

bool TransitionTableWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        for (QWidget* widget = qobject_cast<QWidget*>(watched); widget; widget = widget->parentWidget()) {
            const auto row = std::find(rows_.begin(), rows_.end(), widget);
            if (row != rows_.end()) {
                selectRow(static_cast<int>(std::distance(rows_.begin(), row)));
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TransitionTableWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete && selected_) {
        removeSelected();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && selected_) {
        table_->clearSelection();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

}
