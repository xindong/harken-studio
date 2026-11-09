#include "TransitionTableWidget.h"

#include "Core/ActionStepUtils.h"
#include "Core/AuthoringPolicy.h"
#include "Core/ContextInfo.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeManager.h"
#include "Core/TransitionFieldGating.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/TransitionFade/TransitionFadePreviewField.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
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
        case TransitionExitTiming::Immediate:   return TransitionTableWidget::tr("Immediate");
        case TransitionExitTiming::NaturalEnd:  return TransitionTableWidget::tr("Natural End");
        case TransitionExitTiming::NextBeat:    return TransitionTableWidget::tr("Next Beat");
        case TransitionExitTiming::NextMeasure: return TransitionTableWidget::tr("Next Measure");
        case TransitionExitTiming::ExitMarker:  return TransitionTableWidget::tr("Exit Marker");
    }
    return QString();
}

QString exitTimingDescription(TransitionExitTiming timing)
{
    switch (timing) {
        case TransitionExitTiming::Immediate:   return TransitionTableWidget::tr("Leave the source as soon as the transition is requested.");
        case TransitionExitTiming::NaturalEnd:  return TransitionTableWidget::tr("Leave the source when it finishes playing.");
        case TransitionExitTiming::NextBeat:    return TransitionTableWidget::tr("Leave the source on its next beat.");
        case TransitionExitTiming::NextMeasure: return TransitionTableWidget::tr("Leave the source at the start of its next measure.");
        case TransitionExitTiming::ExitMarker:  return TransitionTableWidget::tr("Leave the source at its exit marker.");
    }
    return QString();
}

QString entryModeText(TransitionEntryMode mode)
{
    return mode == TransitionEntryMode::InitialPosition ? TransitionTableWidget::tr("Initial Position")
                                                        : TransitionTableWidget::tr("Entry Marker");
}

QString entryModeDescription(TransitionEntryMode mode)
{
    return mode == TransitionEntryMode::InitialPosition
        ? TransitionTableWidget::tr("Start the target from its beginning at the transition point.")
        : TransitionTableWidget::tr("Start the target so its entry marker lands on the transition point.");
}

QString effectToolTip(const QString& description, const TransitionOptionEffect& effect)
{
    return effect.note.isEmpty() ? description : description + QStringLiteral("\n\n") + effect.note;
}

void showEffect(QWidget* widget, const QString& description, const TransitionOptionEffect& effect, bool active)
{
    widget->setToolTip(active ? effectToolTip(description, effect) : description);
    QtControlSkin::setSemanticProperty(widget, "inputInvalid", active && !effect.effective());
}

QToolButton* createChevron(QWidget* parent, const QString& name, const QString& toolTip)
{
    auto* chevron = new QToolButton(parent);
    chevron->setObjectName(name);
    chevron->setAutoRaise(true);
    chevron->setArrowType(Qt::DownArrow);
    chevron->setAccessibleName(toolTip);
    chevron->setToolTip(toolTip);
    QtControlSkin::setSemanticProperty(chevron, "flatIconButton", true);
    QtControlSkin::setSemanticProperty(chevron, "navigationRole", QStringLiteral("chevron"));
    return chevron;
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
        {QStringLiteral("Source"), tr("Source"), 300},
        {QStringLiteral("Exit"), tr("Exit"), 200},
        {QStringLiteral("Target"), tr("Target"), 300},
        {QStringLiteral("Entry"), tr("Entry"), 200},
        {QStringLiteral("Toggle"), QString(), 0}
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
        expanded_.clear();
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
    return TransitionRuleEditing::table(boundItem_);
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
    details_.clear();
    rowEndpoints_.clear();

    if (const TransitionTableDef* rules = table()) {
        const std::vector<int> order = TransitionSemantics::rulesForDisplay(*rules);
        for (size_t display = 0; display < order.size(); ++display) {
            const TransitionRuleDef& rule = rules->rules[static_cast<size_t>(order[display])];
            layout->addWidget(buildRow(static_cast<int>(display), rule));
            layout->addWidget(details_.back());
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

    auto* number = table_->createRowNumber(row, displayIndex);
    number->setObjectName(QStringLiteral("transitionRuleNumber"));

    const Endpoints endpoints{rule.source, rule.target};
    const TransitionFieldGating gating = TransitionSemantics::gatingFor(
        *boundItem_, rule, placement_ ? &*placement_ : nullptr, itemResolver());
    auto* details = buildDetails(rule, gating);
    auto* toggleCell = table_->createCell(row);
    toggleCell->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    DebugTint::tintContainer(toggleCell, DebugTint::Role::ColToggle);
    auto* toggle = new QToolButton(toggleCell);
    toggle->setObjectName(QStringLiteral("transitionExpander"));
    toggle->setProperty("flatIconButton", true);
    toggle->setProperty("navigationRole", QStringLiteral("chevron"));
    toggle->setCheckable(true);
    toggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    toggle->setAutoRaise(true);
    const bool expanded = std::find(expanded_.begin(), expanded_.end(), endpoints) != expanded_.end();
    toggle->setChecked(expanded);
    toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    details->setVisible(expanded);
    toggleCell->addWidget(toggle, 0);
    connect(toggle, &QToolButton::toggled, this, [this, toggle, details, endpoints](bool on) {
        details->setVisible(on);
        toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        expanded_.erase(std::remove(expanded_.begin(), expanded_.end(), endpoints), expanded_.end());
        if (on)
            expanded_.push_back(endpoints);
    });

    const QList<QWidget*> cells{
        number,
        buildEndpointCell(row, rule, true),
        buildExitCell(row, rule, gating),
        buildEndpointCell(row, rule, false),
        buildEntryCell(row, rule, gating),
        toggleCell};
    for (QWidget* cell : cells)
        layout->addWidget(cell);
    table_->registerRow(row, cells, details);

    for (QWidget* widget : {row, details}) {
        widget->installEventFilter(this);
        for (QWidget* child : widget->findChildren<QWidget*>())
            child->installEventFilter(this);
    }
    rows_.push_back(row);
    details_.push_back(details);
    return row;
}

QWidget* TransitionTableWidget::buildEndpointCell(QWidget* row, const TransitionRuleDef& rule, bool source)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* chevron = createChevron(cell, source ? QStringLiteral("transitionSourceChevron")
                                               : QStringLiteral("transitionTargetChevron"),
                                  source ? tr("Source options") : tr("Target options"));
    cell->addWidget(chevron, 0);
    if (rule.isDefault()) {
        chevron->setEnabled(false);
        auto* any = new QLabel(QStringLiteral("*"), cell);
        any->setAlignment(Qt::AlignCenter);
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
        setEndpoint(endpoints, source, item ? std::make_optional(item->guid) : std::nullopt);
    });
    cell->addWidget(reference);

    auto* menu = new QMenu(chevron);
    menu->setObjectName(QStringLiteral("transitionEndpointMenu"));
    menu->setToolTipsVisible(true);
    const Endpoints anyEndpoints = source ? Endpoints{std::nullopt, rule.target}
                                          : Endpoints{rule.source, std::nullopt};
    auto* any = menu->addAction(tr("* (Any)"), this, [this, endpoints, source, generation = generation_] {
        if (generation == generation_)
            setEndpoint(endpoints, source, std::nullopt);
    });
    any->setObjectName(QStringLiteral("transitionEndpointAny"));
    any->setCheckable(true);
    any->setChecked(!endpoint);
    const bool anyTaken = endpoint && TransitionRuleEditing::find(boundItem_, anyEndpoints);
    any->setEnabled(endpoint && !anyTaken);
    if (anyTaken)
        any->setToolTip(tr("A rule for this combination already exists"));
    auto* select = menu->addAction(tr("Select Child..."), this, [reference, chevron] {
        reference->openSelector(chevron);
    });
    select->setObjectName(QStringLiteral("transitionEndpointSelectChild"));
    connect(chevron, &QToolButton::clicked, this, [chevron, menu] {
        if (menu->isVisible())
            menu->close();
        else
            menu->popup(chevron->mapToGlobal(QPoint(0, chevron->height())));
    });
    return cell;
}

QWidget* TransitionTableWidget::buildExitCell(QWidget* row, const TransitionRuleDef& rule,
                                              const TransitionFieldGating& gating)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* combo = new QComboBox(cell);
    combo->setObjectName(QStringLiteral("transitionExitTiming"));
    ContextInfo::tag(combo, ContextTopic::forPanel(QStringLiteral("transition.exit")));
    for (TransitionExitTiming timing : AuthoringPolicy::allowedTransitionExitTimings()) {
        combo->addItem(exitTimingText(timing), static_cast<int>(timing));
        combo->setItemData(combo->count() - 1,
                           effectToolTip(exitTimingDescription(timing), gating.exit(timing)), Qt::ToolTipRole);
    }
    combo->setCurrentIndex(combo->findData(static_cast<int>(rule.exitTiming)));
    showEffect(combo, exitTimingDescription(rule.exitTiming), gating.exit(rule.exitTiming), true);
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

QWidget* TransitionTableWidget::buildEntryCell(QWidget* row, const TransitionRuleDef& rule,
                                               const TransitionFieldGating& gating)
{
    auto* cell = table_->createCell(row);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* combo = new QComboBox(cell);
    combo->setObjectName(QStringLiteral("transitionEntryMode"));
    ContextInfo::tag(combo, ContextTopic::forPanel(QStringLiteral("transition.entry")));
    for (TransitionEntryMode mode : {TransitionEntryMode::InitialPosition, TransitionEntryMode::EntryMarker}) {
        combo->addItem(entryModeText(mode), static_cast<int>(mode));
        combo->setItemData(combo->count() - 1,
                           effectToolTip(entryModeDescription(mode), gating.entry(mode)), Qt::ToolTipRole);
    }
    combo->setCurrentIndex(combo->findData(static_cast<int>(rule.entryMode)));
    showEffect(combo, entryModeDescription(rule.entryMode), gating.entry(rule.entryMode), true);
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

QWidget* TransitionTableWidget::buildDetails(const TransitionRuleDef& rule, const TransitionFieldGating& gating)
{
    auto* details = new QWidget(table_->rowsWidget());
    details->setObjectName(QStringLiteral("transitionDetails"));
    DebugTint::tintContainer(details, DebugTint::Role::Accordion);
    auto* layout = new QVBoxLayout(details);
    ThemeLayout::margins(layout, ThemeLayout::Space::None, ThemeLayout::Space::Control,
                         ThemeLayout::Space::Panel, ThemeLayout::Space::Panel);
    ThemeManager::instance().bindMetrics(layout, [layout](const ThemeMetrics& metrics) {
        layout->setSpacing(metrics.inspector.rowSpacing);
    });
    const Endpoints endpoints{rule.source, rule.target};
    const auto addRow = [details, layout](const QString& label, QWidget* control) {
        layout->addWidget(makeInspectorFormRow(details, control->objectName(), label, control, {}, 0).row);
    };
    const auto addSwitch = [&](const QString& name, const QString& topic, const QString& label,
                               const QString& description, const TransitionOptionEffect& effect,
                               const QString& undoText, bool TransitionRuleDef::* field) {
        auto* box = new QCheckBox(details);
        box->setObjectName(name);
        box->setChecked(rule.*field);
        showEffect(box, description, effect, rule.*field);
        ContextInfo::tag(box, ContextTopic::forPanel(topic));
        addRow(label, box);
        connect(box, &QCheckBox::toggled, this,
                [this, endpoints, field, undoText, generation = generation_](bool on) {
            if (generation != generation_)
                return;
            updateRule(endpoints, [&](TransitionRuleDef& rule) { rule.*field = on; }, undoText);
            rebuildRows();
        });
    };
    addSwitch(QStringLiteral("transitionSourceTail"), QStringLiteral("transition.sourceTail"),
              tr("Source Tail"), tr("Keep playing the source content after its exit marker."),
              gating.sourceTail, tr("Set Transition Source Tail"), &TransitionRuleDef::sourceTail);
    addSwitch(QStringLiteral("transitionTargetLeadIn"), QStringLiteral("transition.targetLeadIn"),
              tr("Target Lead-In"), tr("Play the target content before its entry marker ahead of the transition point."),
              gating.targetLeadIn, tr("Set Transition Target Lead-In"), &TransitionRuleDef::targetLeadIn);

    auto* fade = new TransitionFadePreviewField(details);
    fade->setObjectName(QStringLiteral("transitionFade"));
    fade->setPreview(buildTransitionPreview(*boundItem_, rule), rule.hasFade());
    ContextInfo::tag(fade, ContextTopic::forPanel(QStringLiteral("transition.fade")));
    addRow(tr("Fade"), fade);
    connect(fade, &PreviewField::clicked, this, [this, endpoints] {
        if (boundItem_)
            emit externalEditorRequested(
                ExternalEditorSelection::transition(boundItem_->guid, endpoints.first, endpoints.second));
    });
    return details;
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
    const TransitionRuleDef* selectedRule = selected_ ? TransitionRuleEditing::find(boundItem_, *selected_)
                                                      : nullptr;
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
    TransitionRuleDef rule = canonicalTransitionRule();
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
    selfWriting_ = true;
    const bool updated = TransitionRuleEditing::update(boundItem_, endpoints, edit, undoText, this);
    selfWriting_ = false;
    return updated;
}

bool TransitionTableWidget::commit(const TransitionTableDef& candidate, const QString& undoText)
{
    selfWriting_ = true;
    const bool committed = TransitionRuleEditing::commit(boundItem_, candidate, undoText, this);
    selfWriting_ = false;
    return committed;
}

void TransitionTableWidget::setEndpoint(const Endpoints& endpoints, bool source,
                                        const std::optional<QUuid>& guid)
{
    const Endpoints replaced = source ? Endpoints{guid, endpoints.second} : Endpoints{endpoints.first, guid};
    if (updateRule(endpoints, [&](TransitionRuleDef& rule) {
            (source ? rule.source : rule.target) = guid;
        }, tr("Set Transition Endpoints"))) {
        selected_ = replaced;
        if (std::find(expanded_.begin(), expanded_.end(), endpoints) != expanded_.end())
            expanded_.push_back(replaced);
    }
    rebuildRows();
}

bool TransitionTableWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* menu = qobject_cast<QMenu*>(watched);
        menu && menu->objectName() == QStringLiteral("transitionEndpointMenu")
        && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        auto* chevron = menu->parentWidget();
        if (!menu->rect().contains(mouse->position().toPoint()))
            menu->setAttribute(Qt::WA_NoMouseReplay, chevron->rect().contains(
                chevron->mapFromGlobal(mouse->globalPosition().toPoint())));
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        for (QWidget* widget = qobject_cast<QWidget*>(watched); widget; widget = widget->parentWidget()) {
            auto row = std::find(rows_.begin(), rows_.end(), widget);
            if (row == rows_.end())
                row = rows_.begin() + std::distance(details_.begin(),
                                                    std::find(details_.begin(), details_.end(), widget));
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
