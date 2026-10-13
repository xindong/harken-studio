#include "TransitionEditorWidget.h"

#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/ActionStepUtils.h"
#include "Core/Components/VSelection.h"
#include "Core/PlatformContextService.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/CurveSemantics.h"
#include "Core/ItemTypeIcons.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ReferenceSlotResolution.h"
#include "Core/ThemeManager.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/ToastManager.h"
#include "Core/TransitionFieldGating.h"
#include "Core/TransitionRehearsal.h"
#include "Core/VariableRehearsal.h"
#include "TransitionFadeButton.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringList>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

constexpr int kNarrowWidthThreshold = 480;
constexpr int kSourceShare = 280;
constexpr int kExitShare = 220;
constexpr int kEntryShare = 220;

bool isOrDescendsFrom(const ProjectItem* item, const ProjectItem* ancestor)
{
    for (const ProjectItem* current = item; current; current = current->parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

QVariantList placementContextToVariant(const PlacementContext& placement)
{
    QVariantList list;
    for (const PlacementOccurrence& occurrence : placement.occurrences()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("track"), occurrence.trackGuid.toString(QUuid::WithoutBraces));
        entry.insert(QStringLiteral("clip"), occurrence.clipGuid.toString(QUuid::WithoutBraces));
        entry.insert(QStringLiteral("target"), occurrence.targetGuid.toString(QUuid::WithoutBraces));
        list.append(entry);
    }
    return list;
}

std::optional<PlacementContext> placementContextFromVariant(const QVariant& value)
{
    if (!value.isValid())
        return std::nullopt;
    const QVariantList list = value.toList();
    if (list.isEmpty())
        return std::nullopt;
    std::vector<PlacementOccurrence> occurrences;
    occurrences.reserve(static_cast<size_t>(list.size()));
    for (const QVariant& entry : list) {
        const QVariantMap map = entry.toMap();
        const QUuid track = QUuid::fromString(map.value(QStringLiteral("track")).toString());
        const QUuid clip = QUuid::fromString(map.value(QStringLiteral("clip")).toString());
        const QUuid target = QUuid::fromString(map.value(QStringLiteral("target")).toString());
        if (track.isNull() || clip.isNull() || target.isNull())
            return std::nullopt;
        occurrences.push_back(PlacementOccurrence{track, clip, target});
    }
    return PlacementContext(std::move(occurrences));
}

QString conciseNumber(double value)
{
    QString text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString coordinateText(int64_t coordinate, const PieceTimeSpec& time)
{
    if (!AuthoringPolicy::validatePieceTime(time))
        return QObject::tr("Unsupported arrangement");
    const auto seconds = TimelineCoordinates::toSeconds(coordinate, time);
    return seconds ? QObject::tr("%1 s").arg(conciseNumber(*seconds))
                   : QObject::tr("Unknown");
}

QString watchDirectionTag(TransitionSemantics::TransitionWatchDirection direction)
{
    switch (direction) {
        case TransitionSemantics::TransitionWatchDirection::Down:            return QStringLiteral("down");
        case TransitionSemantics::TransitionWatchDirection::UpPlacement:     return QStringLiteral("up-placement");
        case TransitionSemantics::TransitionWatchDirection::UpOrganization: return QStringLiteral("up-organization");
        case TransitionSemantics::TransitionWatchDirection::None: default:  return QStringLiteral("none");
    }
}

TransitionFadeDef* fadeSlotPtr(TransitionRuleDef& rule, TransitionFadeSlot slot)
{
    switch (slot) {
        case TransitionFadeSlot::SourceFadeOut: return &rule.sourceFadeOut;
        case TransitionFadeSlot::BridgeFadeIn:  return &rule.bridgeFadeIn;
        case TransitionFadeSlot::BridgeFadeOut: return &rule.bridgeFadeOut;
        case TransitionFadeSlot::TargetFadeIn:  return &rule.targetFadeIn;
    }
    return &rule.sourceFadeOut;
}

const TransitionFadeDef* fadeSlotPtr(const TransitionRuleDef& rule, TransitionFadeSlot slot)
{
    return fadeSlotPtr(const_cast<TransitionRuleDef&>(rule), slot);
}

QLabel* makeCellLabel(QWidget* parent, const QString& text, const QString& role)
{
    auto* label = new QLabel(text, parent);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setToolTip(text);
    QtControlSkin::setSemanticProperty(label, "textRole", role);
    return label;
}

}

TransitionEditorWidget::TransitionEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TransitionEditorWidget"));
    setFocusPolicy(Qt::StrongFocus);

    const auto& inspector = ThemeManager::instance().metrics().inspector;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(inspector.pageMargin, inspector.pageMargin,
                             inspector.pageMargin, inspector.pageMargin);
    root->setSpacing(inspector.rowSpacing);

    auto* statusBarHost = new QWidget(this);
    statusBarHost->setObjectName(QStringLiteral("transitionStatusBar"));
    statusBarHost->setFixedHeight(inspector.rowHeight);
    statusBarHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* statusBar = new QHBoxLayout(statusBarHost);
    statusBar->setContentsMargins(0, 0, 0, 0);
    statusBar->setSpacing(inspector.columnPadding);
    mountBadge_ = new QLabel(this);
    mountBadge_->setObjectName(QStringLiteral("transitionMountBadge"));
    mountBadge_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar->addWidget(mountBadge_, 0);
    modeLabel_ = new QLabel(this);
    modeLabel_->setObjectName(QStringLiteral("transitionModeLabel"));
    modeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QtControlSkin::setSemanticProperty(modeLabel_, "textRole", QStringLiteral("secondary"));
    statusBar->addWidget(modeLabel_, 1);
    statusSeparator_ = new QLabel(QString(QChar(0x00B7)), statusBarHost);
    QtControlSkin::setSemanticProperty(statusSeparator_, "textRole", QStringLiteral("muted"));
    statusBar->addWidget(statusSeparator_, 0);
    ruleCountLabel_ = new QLabel(this);
    ruleCountLabel_->setObjectName(QStringLiteral("transitionRuleCount"));
    ruleCountLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QtControlSkin::setSemanticProperty(ruleCountLabel_, "textRole", QStringLiteral("muted"));
    QtControlSkin::setSemanticProperty(ruleCountLabel_, "textScale", QStringLiteral("caption"));
    statusBar->addWidget(ruleCountLabel_, 0);
    statusBar->addStretch(1);
    root->addWidget(statusBarHost);

    stack_ = new QStackedWidget(this);
    root->addWidget(stack_, 1);

    stateLabel_ = new QLabel(stack_);
    stateLabel_->setObjectName(QStringLiteral("transitionState"));
    stateLabel_->setAlignment(Qt::AlignCenter);
    stateLabel_->setWordWrap(true);
    stack_->addWidget(stateLabel_);

    scroll_ = new QScrollArea(stack_);
    scroll_->setObjectName(QStringLiteral("transitionScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    contentPage_ = new QWidget(scroll_);
    contentPage_->setObjectName(QStringLiteral("transitionContentPage"));
    auto* contentLayout = new QVBoxLayout(contentPage_);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(inspector.sectionSpacing);

    rulesSection_ = new CollapsibleSection(tr("RULES"), 150, contentPage_);
    rulesSection_->setObjectName(QStringLiteral("transitionRulesSection"));

    mountNoteLabel_ = new QLabel(rulesSection_);
    mountNoteLabel_->setObjectName(QStringLiteral("transitionMountNote"));
    mountNoteLabel_->setWordWrap(true);
    mountNoteLabel_->setContentsMargins(inspector.columnPadding, 0, inspector.columnPadding, inspector.rowSpacing);
    QtControlSkin::setSemanticProperty(mountNoteLabel_, "textRole", QStringLiteral("muted"));
    QtControlSkin::setSemanticProperty(mountNoteLabel_, "textScale", QStringLiteral("caption"));
    mountNoteLabel_->setVisible(false);

    auto* listBox = new TableWidget(rulesSection_);
    listTable_ = listBox;
    listTable_->setSelectionClearHandler([this] { selectRule(-1); });
    listTable_->setRowSelectionHandler([this](QWidget* row) {
        selectRule(row->property("rowIndex").toInt());
    });
    listBox->setColumns(QStringLiteral("Transition"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0, 0, QStringLiteral("transitionHeadNum")},
        {QStringLiteral("Source"), tr("Source"), kSourceShare, 0, QStringLiteral("transitionHeadSource")},
        {QStringLiteral("Target"), tr("Target"), 1000 - kSourceShare - kExitShare - kEntryShare, 0, QStringLiteral("transitionHeadTarget")},
        {QStringLiteral("Exit"), tr("Exit"), kExitShare, 0, QStringLiteral("transitionHeadExit")},
        {QStringLiteral("Entry"), tr("Entry"), kEntryShare, 0, QStringLiteral("transitionHeadEntry")},
        {QStringLiteral("Flags"), QString(), 0}
    });
    listHead_ = listBox->header();
    headNum_ = listBox->headerCell(0);
    headSource_ = listBox->headerCell(1);
    headTarget_ = listBox->headerCell(2);
    headExit_ = listBox->headerCell(3);
    headEntry_ = listBox->headerCell(4);
    headFlags_ = listBox->headerCell(5);
    headNum_->setToolTip(tr("Match order. Row 1 wins over row 2; the default rule is always last."));
    headExit_->setToolTip(tr("When the source leaves. Marker options require a reachable marker reference."));
    headEntry_->setToolTip(tr("How the target comes in, and whether it inherits the source position."));
    listHead_->installEventFilter(this);
    refreshHeadTooltips();
    listRows_ = listBox->rowsWidget();
    auto* foot = listBox->footer();
    addButton_ = foot->addButton();
    addButton_->setObjectName(QStringLiteral("transitionAddRule"));
    removeButton_ = foot->removeButton();
    removeButton_->setObjectName(QStringLiteral("transitionRemoveRule"));

    auto* rulesContent = new QVBoxLayout();
    rulesContent->setContentsMargins(0, 0, 0, 0);
    rulesContent->addWidget(mountNoteLabel_);
    rulesContent->addWidget(listBox);
    rulesSection_->setContentLayout(rulesContent);
    rulesSection_->setDynamicContent(true);
    rulesSection_->setExpanded(true);
    contentLayout->addWidget(rulesSection_);

    sourceExitSection_ = new CollapsibleSection(tr("SOURCE EXIT"), 150, contentPage_);
    sourceExitSection_->setObjectName(QStringLiteral("transitionSourceExitSection"));
    auto* sourceExitLayout = new QVBoxLayout();
    sourceExitLayout->setContentsMargins(0, 0, 0, 0);
    sourceExitLayout->setSpacing(0);

    sourceRef_ = new ObjectRefWidget(sourceExitSection_);
    sourceRef_->setRequired(false);
    sourceRef_->setObjectName(QStringLiteral("transitionSourceRef"));
    sourceRef_->setFilterTypes(EditorUtils::userFacingActionStepTargetTypes(VActionStep::Type::Play));
    sourceRef_->setIsSelectable([this](uint32_t id) {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        return boundItem_ && item
            && ItemTypeCatalog::isUserFacingType(item->type)
            && TransitionSemantics::isEndpointCandidate(*boundItem_, *item);
    });
    sourceRef_->setClearSupport(true);
    sourceAnyLabel_ = new QLabel(tr("Any"), sourceExitSection_);
    sourceAnyLabel_->setObjectName(QStringLiteral("transitionSourceAny"));
    QtControlSkin::setSemanticProperty(sourceAnyLabel_, "textRole", QStringLiteral("secondary"));
    sourceAnyLabel_->setToolTip(
        TransitionSemantics::defaultRuleEndpointReason());
    sourceAnyLabel_->setVisible(false);
    {
        auto* stackWidget = new QWidget(sourceExitSection_);
        auto* stackLayout = new QHBoxLayout(stackWidget);
        stackLayout->setContentsMargins(0, 0, 0, 0);
        stackLayout->addWidget(sourceRef_);
        stackLayout->addWidget(sourceAnyLabel_);
        sourceExitLayout->addWidget(
            makeInspectorFormRow(sourceExitSection_, QStringLiteral("source"),
                                 tr("Source"), stackWidget).row);
    }

    exitTimingCombo_ = new QComboBox(sourceExitSection_);
    exitTimingCombo_->setObjectName(QStringLiteral("transitionExitTiming"));
    for (TransitionExitTiming timing : AuthoringPolicy::allowedTransitionExitTimings()) {
        exitTimingCombo_->addItem(exitTimingText(timing), static_cast<int>(timing));
    }
    sourceExitLayout->addWidget(
        makeInspectorFormRow(sourceExitSection_, QStringLiteral("exitTiming"),
                             tr("Exit timing"), exitTimingCombo_).row);

    sourceFadeButton_ = new TransitionFadeButton(sourceExitSection_);
    sourceFadeButton_->setObjectName(QStringLiteral("transitionFadeSourceOut"));
    sourceExitLayout->addWidget(
        makeInspectorFormRow(sourceExitSection_, QStringLiteral("sourceFadeOut"),
                             tr("Fade out"), sourceFadeButton_, Qt::AlignLeft).row);

    exitNoteLabel_ = new QLabel(sourceExitSection_);
    exitNoteLabel_->setObjectName(QStringLiteral("transitionExitTimingNote"));
    exitNoteLabel_->setWordWrap(true);
    exitNoteLabel_->setContentsMargins(inspector.columnPadding, 0, inspector.columnPadding, 0);
    exitNoteLabel_->setVisible(false);
    sourceExitLayout->addWidget(exitNoteLabel_);

    sourceExitSection_->setContentLayout(sourceExitLayout);
    sourceExitSection_->setDynamicContent(true);
    contentLayout->addWidget(sourceExitSection_);

    bridgeSection_ = new CollapsibleSection(tr("BRIDGE"), 150, contentPage_);
    bridgeSection_->setObjectName(QStringLiteral("transitionBridgeSection"));
    auto* bridgeSectionLayout = new QVBoxLayout();
    bridgeSectionLayout->setContentsMargins(0, 0, 0, 0);
    bridgeSectionLayout->setSpacing(0);

    bridgeSlotAdd_ = new QWidget(bridgeSection_);
    bridgeSlotAdd_->setObjectName(QStringLiteral("transitionBridgeSlotAdd"));
    bridgeSlotAdd_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    {
        auto* slotLayout = new QHBoxLayout(bridgeSlotAdd_);
        slotLayout->setContentsMargins(0, 0, 0, 0);
        slotLayout->setSpacing(8);
        bridgeInsertButton_ = new QToolButton(bridgeSlotAdd_);
        bridgeInsertButton_->setObjectName(QStringLiteral("transitionInsertBridge"));
        bridgeInsertButton_->setText(tr("+ Insert a bridge"));
        bridgeInsertButton_->setAutoRaise(true);
        bridgeInsertButton_->setCursor(Qt::PointingHandCursor);
        bridgeInsertButton_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        slotLayout->addWidget(bridgeInsertButton_, 0);
        auto* note = new QLabel(
            tr("Any content object"), bridgeSlotAdd_);
        note->setObjectName(QStringLiteral("transitionBridgeSlotNote"));
        note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        note->setToolTip(note->text());
        QtControlSkin::setSemanticProperty(note, "textRole", QStringLiteral("muted"));
        slotLayout->addWidget(note, 1);
        slotLayout->addStretch(1);
        connect(bridgeInsertButton_, &QToolButton::clicked, this, [this]() {
            bridgeFormRevealed_ = true;
            rebuildDetail();
        });
    }
    bridgeSectionLayout->addWidget(bridgeSlotAdd_);

    bridgeForm_ = new QWidget(bridgeSection_);
    bridgeForm_->setObjectName(QStringLiteral("transitionBridgeForm"));
    {
        auto* formLayout = new QVBoxLayout(bridgeForm_);
        formLayout->setContentsMargins(0, 0, 0, 0);
        formLayout->setSpacing(0);

        bridgeRef_ = new ObjectRefWidget(bridgeForm_);
        bridgeRef_->setRequired(false);
        bridgeRef_->setObjectName(QStringLiteral("transitionBridgeRef"));
        bridgeRef_->setFilterTypes(EditorUtils::userFacingActionStepTargetTypes(VActionStep::Type::Play));
        bridgeRef_->setIsSelectable([](uint32_t id) {
            ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
            return item
                && ItemTypeCatalog::isUserFacingType(item->type)
                && TransitionSemantics::isBridgeCandidate(*item);
        });
        bridgeRef_->setClearSupport(true);
        formLayout->addWidget(
            makeInspectorFormRow(bridgeForm_, QStringLiteral("bridge"),
                                 tr("Bridge"), bridgeRef_).row);

        bridgeFadeInButton_ = new TransitionFadeButton(bridgeForm_);
        bridgeFadeInButton_->setObjectName(QStringLiteral("transitionFadeBridgeIn"));
        formLayout->addWidget(
            makeInspectorFormRow(bridgeForm_, QStringLiteral("bridgeFadeIn"),
                                 tr("Fade in"), bridgeFadeInButton_, Qt::AlignLeft).row);

        bridgeFadeOutButton_ = new TransitionFadeButton(bridgeForm_);
        bridgeFadeOutButton_->setObjectName(QStringLiteral("transitionFadeBridgeOut"));
        formLayout->addWidget(
            makeInspectorFormRow(bridgeForm_, QStringLiteral("bridgeFadeOut"),
                                 tr("Fade out"), bridgeFadeOutButton_, Qt::AlignLeft).row);
    }
    bridgeForm_->setVisible(false);
    bridgeSectionLayout->addWidget(bridgeForm_);

    bridgeSection_->setContentLayout(bridgeSectionLayout);
    bridgeSection_->setDynamicContent(true);
    contentLayout->addWidget(bridgeSection_);

    targetEntrySection_ = new CollapsibleSection(tr("TARGET ENTRY"), 150, contentPage_);
    targetEntrySection_->setObjectName(QStringLiteral("transitionTargetEntrySection"));
    auto* targetEntryLayout = new QVBoxLayout();
    targetEntryLayout->setContentsMargins(0, 0, 0, 0);
    targetEntryLayout->setSpacing(0);

    targetRef_ = new ObjectRefWidget(targetEntrySection_);
    targetRef_->setRequired(false);
    targetRef_->setObjectName(QStringLiteral("transitionTargetRef"));
    targetRef_->setFilterTypes(EditorUtils::userFacingActionStepTargetTypes(VActionStep::Type::Play));
    targetRef_->setIsSelectable([this](uint32_t id) {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        return boundItem_ && item
            && ItemTypeCatalog::isUserFacingType(item->type)
            && TransitionSemantics::isEndpointCandidate(*boundItem_, *item);
    });
    targetRef_->setClearSupport(true);
    targetAnyLabel_ = new QLabel(tr("Any"), targetEntrySection_);
    targetAnyLabel_->setObjectName(QStringLiteral("transitionTargetAny"));
    QtControlSkin::setSemanticProperty(targetAnyLabel_, "textRole", QStringLiteral("secondary"));
    targetAnyLabel_->setToolTip(
        TransitionSemantics::defaultRuleEndpointReason());
    targetAnyLabel_->setVisible(false);
    {
        auto* stackWidget = new QWidget(targetEntrySection_);
        auto* stackLayout = new QHBoxLayout(stackWidget);
        stackLayout->setContentsMargins(0, 0, 0, 0);
        stackLayout->addWidget(targetRef_);
        stackLayout->addWidget(targetAnyLabel_);
        targetEntryLayout->addWidget(
            makeInspectorFormRow(targetEntrySection_, QStringLiteral("target"),
                                 tr("Target"), stackWidget).row);
    }

    entryModeCombo_ = new QComboBox(targetEntrySection_);
    entryModeCombo_->setObjectName(QStringLiteral("transitionEntryMode"));
    for (int i = 0; i <= static_cast<int>(TransitionEntryMode::SamePosition); ++i) {
        const auto mode = static_cast<TransitionEntryMode>(i);
        entryModeCombo_->addItem(entryModeText(mode), static_cast<int>(mode));
    }
    targetEntryLayout->addWidget(
        makeInspectorFormRow(targetEntrySection_, QStringLiteral("entryMode"),
                             tr("Entry mode"), entryModeCombo_).row);

    targetFadeInButton_ = new TransitionFadeButton(targetEntrySection_);
    targetFadeInButton_->setObjectName(QStringLiteral("transitionFadeTargetIn"));
    targetEntryLayout->addWidget(
        makeInspectorFormRow(targetEntrySection_, QStringLiteral("targetFadeIn"),
                             tr("Fade in"), targetFadeInButton_, Qt::AlignLeft).row);

    positionValue_ = new QLabel(targetEntrySection_);
    positionValue_->setObjectName(QStringLiteral("transitionSamePosition"));
    positionValue_->setWordWrap(true);
    positionValue_->setProperty("roValue", true);
    const InspectorFormRow positionFormRow = makeInspectorFormRow(
        targetEntrySection_, QStringLiteral("position"), tr("Position"), positionValue_,
        Qt::AlignLeft);
    positionRow_ = positionFormRow.row;
    targetEntryLayout->addWidget(positionRow_);

    targetEntrySection_->setContentLayout(targetEntryLayout);
    targetEntrySection_->setDynamicContent(true);
    contentLayout->addWidget(targetEntrySection_);

    resolutionSection_ = new CollapsibleSection(tr("RESOLUTION"), 150, contentPage_);
    resolutionSection_->setObjectName(QStringLiteral("transitionResolutionSection"));
    auto* resolutionLayout = new QVBoxLayout();
    resolutionLayout->setContentsMargins(0, 0, 0, 0);
    resolutionLayout->setSpacing(0);

    watchValue_ = new QLabel(resolutionSection_);
    watchValue_->setObjectName(QStringLiteral("transitionWatch"));
    watchValue_->setWordWrap(true);
    watchValue_->setProperty("roValue", true);
    resolutionLayout->addWidget(
        makeInspectorFormRow(resolutionSection_, QStringLiteral("watch"),
                             tr("Watch"), watchValue_, Qt::AlignLeft).row);

    handoverValue_ = new QLabel(resolutionSection_);
    handoverValue_->setObjectName(QStringLiteral("transitionHandover"));
    handoverValue_->setWordWrap(true);
    handoverValue_->setProperty("roValue", true);
    resolutionLayout->addWidget(
        makeInspectorFormRow(resolutionSection_, QStringLiteral("handover"),
                             tr("Hand-over"), handoverValue_, Qt::AlignLeft).row);

    spliceValue_ = new QLabel(resolutionSection_);
    spliceValue_->setObjectName(QStringLiteral("transitionSplice"));
    spliceValue_->setWordWrap(true);
    spliceValue_->setProperty("roValue", true);
    spliceValue_->setText(tr("This queue advances on its splice marker."));
    spliceValue_->setToolTip(tr(
        "How this table's own hand-over rules interact with the queue's marker "
        "splice is not decided here."));
    spliceRow_ = makeInspectorFormRow(resolutionSection_, QStringLiteral("splice"),
                                      tr("Splice"), spliceValue_, Qt::AlignLeft).row;
    resolutionLayout->addWidget(spliceRow_);

    resolutionSection_->setContentLayout(resolutionLayout);
    resolutionSection_->setDynamicContent(true);
    contentLayout->addWidget(resolutionSection_);

    contentLayout->addStretch(1);
    scroll_->setWidget(contentPage_);
    stack_->addWidget(scroll_);

    connect(addButton_, &QToolButton::clicked, this, &TransitionEditorWidget::addUnusedRule);
    connect(removeButton_, &QToolButton::clicked, this, &TransitionEditorWidget::removeSelectedRule);

    connect(sourceRef_, &ObjectRefWidget::objectChanged, this, [this](uint32_t id) {
        if (selectedDisplayIndex_ < 0) return;
        ProjectItem* item = id != 0 ? ProjectManager::instance().findItemByObjectID(id) : nullptr;
        setRuleEndpoints(selectedDisplayIndex_, item ? std::make_optional(item->guid) : std::nullopt,
                         ruleTarget(selectedDisplayIndex_));
    });
    connect(targetRef_, &ObjectRefWidget::objectChanged, this, [this](uint32_t id) {
        if (selectedDisplayIndex_ < 0) return;
        ProjectItem* item = id != 0 ? ProjectManager::instance().findItemByObjectID(id) : nullptr;
        setRuleEndpoints(selectedDisplayIndex_, ruleSource(selectedDisplayIndex_),
                         item ? std::make_optional(item->guid) : std::nullopt);
    });
    connect(bridgeRef_, &ObjectRefWidget::objectChanged, this, [this](uint32_t id) {
        if (selectedDisplayIndex_ < 0) return;
        ProjectItem* item = id != 0 ? ProjectManager::instance().findItemByObjectID(id) : nullptr;
        bridgeFormRevealed_ = true;
        setRuleBridge(selectedDisplayIndex_, item ? std::make_optional(item->guid) : std::nullopt);
    });
    connect(exitTimingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
           [this](int index) {
        if (selectedDisplayIndex_ < 0 || index < 0) return;
        const QVariant data = exitTimingCombo_->itemData(index);
        if (!data.isValid()) return;
        setRuleExitTiming(selectedDisplayIndex_, static_cast<TransitionExitTiming>(data.toInt()));
    });
    connect(entryModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
           [this](int index) {
        if (selectedDisplayIndex_ < 0 || index < 0) return;
        const QVariant data = entryModeCombo_->itemData(index);
        if (!data.isValid()) return;
        setRuleEntryMode(selectedDisplayIndex_, static_cast<TransitionEntryMode>(data.toInt()));
    });
    connect(sourceFadeButton_, &QToolButton::clicked, this, [this]() {
        openFadePopup(TransitionFadeSlot::SourceFadeOut, sourceFadeButton_);
    });
    connect(bridgeFadeInButton_, &QToolButton::clicked, this, [this]() {
        openFadePopup(TransitionFadeSlot::BridgeFadeIn, bridgeFadeInButton_);
    });
    connect(bridgeFadeOutButton_, &QToolButton::clicked, this, [this]() {
        openFadePopup(TransitionFadeSlot::BridgeFadeOut, bridgeFadeOutButton_);
    });
    connect(targetFadeInButton_, &QToolButton::clicked, this, [this]() {
        openFadePopup(TransitionFadeSlot::TargetFadeIn, targetFadeInButton_);
    });

    for (CollapsibleSection* section : {sourceExitSection_, bridgeSection_,
                                        targetEntrySection_, resolutionSection_}) {
        connect(section, &CollapsibleSection::expandedChanged, this, [this](bool) {
            if (!applyingCollapse_)
                collapseDirty_ = true;
        });
    }

    connect(&ProjectManager::instance().variableRehearsal(), &VariableRehearsal::rehearsalChanged,
            this, [this](const QUuid&) { refreshRehearsalWinner(); });
    connect(&ProjectManager::instance().transitionRehearsal(), &TransitionRehearsal::rehearsalChanged,
           this, [this](const QUuid& containerId) {
        if (boundItem_ && containerId == boundItem_->guid) {
            handoverValue_->setText(handoverReadoutText());
    handoverValue_->setToolTip(isQueueMode()
        ? tr("Illustrative only - the real queue order is not predicted here.")
        : QString());
            refreshWatchView();
        }
    });

    auto& manager = ProjectManager::instance();
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) { rebuild(); });
    connect(&manager, &ProjectManager::itemCreated, this, [this](ProjectItem* item) {
        if (item && item->parent == boundItem_)
            rebuild();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemMoved, this,
            [this](ProjectItem* item, ProjectItem* oldParent, ProjectItem* newParent) {
        if (item == boundItem_ || oldParent == boundItem_ || newParent == boundItem_)
            rebuild();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemRenamed, this,
            [this](ProjectItem* item, const QString&) {
        if (item && (item == boundItem_ || item->parent == boundItem_))
            rebuild();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject* originator) {
        if (item != boundItem_)
            return;
        if (property == QStringLiteral("transitions")) {
            // commitTable() already applies this widget's own edits synchronously
            // (rebuildRuleList/refreshRehearsalWinner/refreshWatchView); only an
            // externally-originated change (another panel, IPC, undo/redo) needs
            // the full rebuild here.
            if (originator != this)
                rebuild();
            return;
        }
        if (property == QStringLiteral("mode")
            || property == QStringLiteral("selections")
            || property == QStringLiteral("playType")
            || property == QStringLiteral("variableResponse")
            || property == QStringLiteral("spliceByMarker"))
            rebuild();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (!item)
            return;
        if (isOrDescendsFrom(boundItem_, item)) {
            clearEditor();
            return;
        }
        if (!boundItem_)
            return;
        if (isOrDescendsFrom(item, boundItem_) || tableReferences(item->guid))
            rebuild();
    });
    connect(&manager, &ProjectManager::projectClosed, this, [this] {
        boundItem_ = nullptr;
        showState({});
        refreshWatchView();
    });
    connect(&manager, &ProjectManager::projectOpened, this, [this](ProjectData*) {
        if (boundGuid_.isNull())
            return;
        boundItem_ = ProjectManager::instance().findItemByGuid(boundGuid_);
        if (!boundItem_) {
            showState({});
            return;
        }
        if (placementContext_) {
            const ReferenceSlotResolution::ResolutionScope scope =
                ReferenceSlotResolution::buildResolutionScope(
                    boundItem_, *placementContext_, itemResolver());
            if (!scope.placementContextValid)
                placementContext_.reset();
        }
        rebuild();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        refreshStatusBar();
        if (boundItem_)
            rebuildRuleList();
    });

    clearEditor();
}

bool TransitionEditorWidget::acceptsItem(const ProjectItem* item)
{
    return item && acceptsItemType(item->type);
}

bool TransitionEditorWidget::acceptsItemType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Container;
}

void TransitionEditorWidget::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

QString TransitionEditorWidget::stateText() const
{
    return stateLabel_->text();
}

const TransitionTableDef* TransitionEditorWidget::table() const
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    return selection ? &selection->transitions : nullptr;
}

int TransitionEditorWidget::ruleCount() const
{
    const auto* t = table();
    return t ? static_cast<int>(t->rules.size()) : 0;
}

const TransitionRuleDef* TransitionEditorWidget::ruleAt(int displayIndex) const
{
    const auto* t = table();
    if (!t)
        return nullptr;
    const std::vector<int> order = TransitionSemantics::rulesForDisplay(*t);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(order.size()))
        return nullptr;
    return &t->rules[order[displayIndex]];
}

bool TransitionEditorWidget::ruleIsDefault(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule && rule->isDefault();
}

int TransitionEditorWidget::ruleSpecificity(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? TransitionSemantics::ruleSpecificity(*rule) : -1;
}

std::optional<QUuid> TransitionEditorWidget::ruleSource(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? rule->source : std::nullopt;
}

std::optional<QUuid> TransitionEditorWidget::ruleTarget(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? rule->target : std::nullopt;
}

TransitionExitTiming TransitionEditorWidget::ruleExitTiming(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? rule->exitTiming : TransitionExitTiming::Immediate;
}

TransitionEntryMode TransitionEditorWidget::ruleEntryMode(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? rule->entryMode : TransitionEntryMode::EntryMarker;
}

QString TransitionEditorWidget::ruleSummaryText(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    if (!rule)
        return QString();
    const QString bridgeText = rule->bridge
        ? tr(" - bridge %1").arg(endpointName(rule->bridge)) : QString();
    return tr("%1 -> %2 - exit %3%4 - entry %5")
        .arg(endpointName(rule->source), endpointName(rule->target),
             exitTimingText(rule->exitTiming), bridgeText,
             entryModeText(rule->entryMode));
}

int TransitionEditorWidget::selectedRuleIndex() const
{
    return selectedDisplayIndex_;
}

bool TransitionEditorWidget::selectRuleDisplayIndex(int displayIndex)
{
    if (displayIndex < 0 || displayIndex >= static_cast<int>(rowWidgets_.size()))
        return false;
    selectRule(displayIndex);
    return true;
}

void TransitionEditorWidget::selectRule(int displayIndex)
{
    bridgeFormRevealed_ = false;
    selectedDisplayIndex_ = displayIndex;
    for (size_t i = 0; i < rowWidgets_.size(); ++i) {
        QWidget* row = rowWidgets_[i];
        const bool selected = static_cast<int>(i) == displayIndex;
        row->setProperty("selected", selected);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    const TransitionRuleDef* rule = displayIndex >= 0 ? ruleAt(displayIndex) : nullptr;
    selectedEndpoints_ = rule
        ? std::make_optional(Endpoints{rule->source, rule->target}) : std::nullopt;
    if (rule)
        setFocus(Qt::OtherFocusReason);
    rebuildDetail();
}

void TransitionEditorWidget::refreshFooterGating()
{
    const TransitionTableDef* t = table();
    if (!boundItem_ || !t) {
        addButton_->setEnabled(false);
        removeButton_->setEnabled(false);
        return;
    }
    const TransitionLedgerGating ledger = TransitionSemantics::ledgerGatingFor(
        *boundItem_, *t,
        selectedDisplayIndex_ >= 0 ? ruleAt(selectedDisplayIndex_) : nullptr,
        itemResolver());

    const bool canAdd = ledger.addRule == FieldAvail::Available;
    addButton_->setEnabled(canAdd);
    addButton_->setToolTip(canAdd ? QString() : ledger.reasonAddRule);

    const bool canRemove = ledger.removeRule == FieldAvail::Available;
    removeButton_->setEnabled(canRemove);
    removeButton_->setToolTip(canRemove ? QString() : ledger.reasonRemoveRule);
}

std::vector<ProjectItem*> TransitionEditorWidget::candidateChildren() const
{
    std::vector<ProjectItem*> result;
    if (!boundItem_ || boundItem_->type != ProjectItem::Type::Container)
        return result;
    result.reserve(boundItem_->children.size());
    for (const auto& child : boundItem_->children) {
        if (child && AuthoringPolicy::validateNewSubtree(*child, nullptr, itemResolver()))
            result.push_back(child.get());
    }
    return result;
}

int TransitionEditorWidget::candidateCount() const
{
    return static_cast<int>(candidateChildren().size());
}

QUuid TransitionEditorWidget::candidateGuid(int index) const
{
    const std::vector<ProjectItem*> children = candidateChildren();
    return index >= 0 && index < static_cast<int>(children.size())
        ? children[static_cast<size_t>(index)]->guid : QUuid();
}

bool TransitionEditorWidget::isCombinationOccupied(const std::optional<QUuid>& source,
                                                   const std::optional<QUuid>& target) const
{
    if (!source && !target)
        return true;
    const auto* t = table();
    if (!t)
        return false;
    return std::any_of(t->rules.begin(), t->rules.end(), [&](const TransitionRuleDef& rule) {
        return rule.source == source && rule.target == target;
    });
}

std::optional<TransitionEditorWidget::Endpoints>
TransitionEditorWidget::firstUnusedCombination() const
{
    const TransitionTableDef* t = table();
    if (!boundItem_ || !t)
        return std::nullopt;
    return TransitionSemantics::firstUnusedCombination(*boundItem_, *t, itemResolver());
}

bool TransitionEditorWidget::addRule(const std::optional<QUuid>& source,
                                     const std::optional<QUuid>& target)
{
    const auto* current = table();
    if (!current) {
        rejectInteraction(tr("No transition table is bound."));
        return false;
    }
    TransitionRuleDef rule = AuthoringPolicy::defaultTransitionRule();
    rule.source = source;
    rule.target = target;
    TransitionTableDef candidate = *current;
    const TransitionValidation validation = candidate.addRule(rule);
    if (!validation) {
        rejectInteraction(validation.message);
        return false;
    }
    selectedEndpoints_ = Endpoints{source, target};
    return commitTable(std::move(candidate), tr("Add Transition Rule"));
}

bool TransitionEditorWidget::addUnusedRule()
{
    const std::optional<Endpoints> combo = firstUnusedCombination();
    if (!combo) {
        rejectInteraction(candidateChildren().empty()
            ? tr("This Container has no children to reference.")
            : tr("Every source and target combination already has a rule."));
        return false;
    }
    return addRule(combo->first, combo->second);
}

bool TransitionEditorWidget::removeRule(int displayIndex)
{
    const auto* current = table();
    if (!current) {
        rejectInteraction(tr("No transition table is bound."));
        return false;
    }
    const std::vector<int> order = TransitionSemantics::rulesForDisplay(*current);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(order.size())) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionTableDef candidate = *current;
    const TransitionValidation validation = candidate.removeRule(order[displayIndex]);
    if (!validation) {
        rejectInteraction(validation.message);
        return false;
    }
    auto remove = [this, candidate = std::move(candidate)]() mutable {
        return commitTable(std::move(candidate), tr("Remove Transition Rule"));
    };
    return displayIndex == selectedRuleIndex() ? listTable_->removeSelectedRow(remove) : remove();
}

bool TransitionEditorWidget::removeSelectedRule()
{
    return removeRule(selectedRuleIndex());
}

QVariantMap TransitionEditorWidget::snapshotState() const
{
    QVariantMap state;
    if (!boundGuid_.isNull())
        state.insert(QStringLiteral("itemGuid"), boundGuid_.toString(QUuid::WithoutBraces));
    if (placementContext_)
        state.insert(QStringLiteral("placement"), placementContextToVariant(*placementContext_));
    return state;
}

void TransitionEditorWidget::bindItem(ProjectItem* item)
{
    boundGuid_ = item ? item->guid : QUuid();
    boundItem_ = item;
    lastInteractionError_.clear();
    listTable_->clearSelection();
    placementContext_.reset();
    collapseDirty_ = false;
    rebuild();
}

void TransitionEditorWidget::bindGuid(const QUuid& guid)
{
    boundGuid_ = guid;
    boundItem_ = guid.isNull() ? nullptr : ProjectManager::instance().findItemByGuid(guid);
    lastInteractionError_.clear();
    listTable_->clearSelection();
    placementContext_.reset();
    collapseDirty_ = false;
    if (!boundItem_ && !guid.isNull()) {
        showState({});
        refreshWatchView();
        refreshSpliceReadout();
        return;
    }
    rebuild();
}

void TransitionEditorWidget::setPlacementContext(const std::optional<PlacementContext>& placement)
{
    placementContext_ = placement;
    refreshWatchView();
}

const PlacementContext* TransitionEditorWidget::placementContext() const
{
    return placementContext_ ? &*placementContext_ : nullptr;
}

void TransitionEditorWidget::clearEditor()
{
    boundGuid_ = QUuid();
    boundItem_ = nullptr;
    lastInteractionError_.clear();
    listTable_->clearSelection();
    placementContext_.reset();
    collapseDirty_ = false;
    rebuild();
}

void TransitionEditorWidget::restoreState(const QVariantMap& state)
{
    bindGuid(QUuid::fromString(state.value(QStringLiteral("itemGuid")).toString()));

    std::optional<PlacementContext> placement =
        placementContextFromVariant(state.value(QStringLiteral("placement")));
    if (placement && boundItem_) {
        const ReferenceSlotResolution::ResolutionScope scope =
            ReferenceSlotResolution::buildResolutionScope(boundItem_, *placement, itemResolver());
        if (!scope.placementContextValid)
            placement.reset();
    } else {
        placement.reset();
    }
    setPlacementContext(placement);
}

void TransitionEditorWidget::rebuild()
{
    sourceRef_->setDirectChildParentGuid(boundGuid_);
    targetRef_->setDirectChildParentGuid(boundGuid_);
    if (!boundItem_) {
        mountState_ = TransitionMountState::None;
        showState({});
        refreshStatusBar();
        refreshWatchView();
        refreshSpliceReadout();
        return;
    }
    if (boundItem_->type != ProjectItem::Type::Container) {
        mountState_ = TransitionMountState::None;
        showState(tr("%1 cannot be edited in Transition Editor. Select a Container.")
                      .arg(boundItem_->name));
        refreshStatusBar();
        refreshWatchView();
        refreshSpliceReadout();
        return;
    }

    mountState_ = TransitionSemantics::transitionMountState(*boundItem_,
        currentWorldPlatformScope(PlatformContextService::instance().context()));
    stateLabel_->setText(mountStateExplanation());
    refreshStatusBar();

    // D5: None means "not consumed by the runtime", not "read-only" - the
    // table stays visible and editable in every mount state so an author can
    // prepare it before switching modes. The strip above the table explains
    // why, matching Idle's presentation.
    mountNoteLabel_->setText(mountNoteText());
    mountNoteLabel_->setVisible(mountState_ != TransitionMountState::Editable);

    stack_->setCurrentWidget(scroll_);
    applyDefaultCollapse(true);
    rebuildRuleList();
    refreshRehearsalWinner();
    refreshWatchView();
    refreshSpliceReadout();
}

void TransitionEditorWidget::rebuildRuleList()
{
    for (QWidget* row : rowWidgets_)
        delete row;
    rowWidgets_.clear();

    const auto* t = table();
    if (!t) {
        selectedDisplayIndex_ = -1;
        const QString countText = tr("%n rule(s)", nullptr, 0);
        ruleCountLabel_->setText(countText);
        ruleCountLabel_->setToolTip(countText);
        rebuildDetail();
        return;
    }

    auto* rowsLayout = qobject_cast<QVBoxLayout*>(listRows_->layout());
    const std::vector<int> order = TransitionSemantics::rulesForDisplay(*t);
    for (size_t display = 0; display < order.size(); ++display) {
        const TransitionRuleDef& rule = t->rules[order[display]];

        auto* row = new QWidget(listRows_);
        row->setObjectName(QStringLiteral("transitionListRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setProperty("listRow", true);
        row->setProperty("lastRow", display + 1 == order.size());
        row->setProperty("rowIndex", static_cast<int>(display));
        row->setCursor(Qt::PointingHandCursor);
        DebugTint::tint(row, DebugTint::Role::Row);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        auto* numLabel = makeCellLabel(row, QString::number(static_cast<int>(display) + 1),
                                       QStringLiteral("muted"));
        numLabel->setObjectName(QStringLiteral("transitionRuleNum"));
        numLabel->setToolTip(QString());
        DebugTint::tint(numLabel, DebugTint::Role::Label);
        rowLayout->addWidget(numLabel);

        QLabel* sourceCell = makeEndpointCell(row, rule.source);
        rowLayout->addWidget(sourceCell);

        QLabel* targetCell = makeEndpointCell(row, rule.target);
        rowLayout->addWidget(targetCell);

        QLabel* exitCell =
            makeCellLabel(row, exitTimingText(rule.exitTiming), QStringLiteral("primary"));
        rowLayout->addWidget(exitCell);

        QLabel* entryCell =
            makeCellLabel(row, entryModeText(rule.entryMode), QStringLiteral("primary"));
        rowLayout->addWidget(entryCell);

        auto* flagsCell = listTable_->createCell(row);
        if (rule.isDefault()) {
            auto* lock = new QLabel(QString(QChar(0x2261)), flagsCell);
            lock->setAlignment(Qt::AlignCenter);
            QtControlSkin::setSemanticProperty(lock, "textRole", QStringLiteral("muted"));
            lock->setToolTip(TransitionSemantics::defaultRuleEndpointReason());
            flagsCell->addWidget(lock);
        }
        if (rule.bridge.has_value()) {
            auto* bridge = new QLabel(QString(QChar(0x25C6)), flagsCell);
            bridge->setAlignment(Qt::AlignCenter);
            QtControlSkin::setSemanticProperty(bridge, "textRole", QStringLiteral("accent"));
            bridge->setToolTip(tr("Bridge: %1").arg(endpointName(rule.bridge)));
            flagsCell->addWidget(bridge);
        }
        rowLayout->addWidget(flagsCell);

        row->installEventFilter(this);
        flagsCell->installEventFilter(this);

        listTable_->registerRow(row, {numLabel, sourceCell, targetCell, exitCell, entryCell, flagsCell});
        rowsLayout->addWidget(row);
        rowWidgets_.push_back(row);
    }

    const QString countText = tr("%n rule(s)", nullptr, static_cast<int>(order.size()));
    ruleCountLabel_->setText(countText);
    ruleCountLabel_->setToolTip(countText);
    listTable_->refreshLayout();
    applyDefaultCollapse(false);

    int selectIndex = -1;
    if (selectedEndpoints_) {
        for (size_t display = 0; display < order.size(); ++display) {
            const TransitionRuleDef& rule = t->rules[order[display]];
            if (rule.source == selectedEndpoints_->first
                && rule.target == selectedEndpoints_->second) {
                selectIndex = static_cast<int>(display);
                break;
            }
        }
    }
    selectedDisplayIndex_ = -1;
    if (selectIndex >= 0) {
        selectRule(selectIndex);
    } else {
        selectedEndpoints_.reset();
        rebuildDetail();
    }
}

bool TransitionEditorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == listHead_ && event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        if (QLayout* layout = listHead_->layout()) {
            for (int i = 0; i < layout->count(); ++i) {
                QWidget* child = layout->itemAt(i)->widget();
                if (!child || !child->geometry().contains(help->pos()))
                    continue;
                const QString tip = child->toolTip();
                if (!tip.isEmpty()) {
                    QToolTip::showText(help->globalPos(), tip, listHead_);
                    return true;
                }
                break;
            }
        }
        QToolTip::hideText();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::ContextMenu) {
        auto* w = qobject_cast<QWidget*>(watched);
        QWidget* rowHost = w;
        while (rowHost
               && std::find(rowWidgets_.begin(), rowWidgets_.end(), rowHost) == rowWidgets_.end())
            rowHost = rowHost->parentWidget();
        if (rowHost) {
            const auto it = std::find(rowWidgets_.begin(), rowWidgets_.end(), rowHost);
            selectRule(static_cast<int>(std::distance(rowWidgets_.begin(), it)));
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TransitionEditorWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && selectedDisplayIndex_ >= 0) {
        listTable_->clearSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete && selectedDisplayIndex_ >= 0) {
        removeSelectedRule();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TransitionEditorWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshChrome();
    applyDefaultCollapse(false);
}

void TransitionEditorWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    applyDefaultCollapse(false);
}

bool TransitionEditorWidget::isNarrow() const
{
    return width() > 0 && width() < kNarrowWidthThreshold;
}

void TransitionEditorWidget::refreshHeadTooltips()
{
    if (!headSource_ || !headTarget_)
        return;
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    if (selection && selection->mode == SelectionMode::Match) {
        headSource_->setToolTip(tr(
            "The winning row before the discrete grid re-evaluates. "
            "More specific rules win: exact source beats Any."));
        headTarget_->setToolTip(tr("The new winning row after re-evaluation."));
    } else if (isQueueMode()) {
        headSource_->setToolTip(tr(
            "The item playing before the queue advances. "
            "More specific rules win: exact source beats Any."));
        headTarget_->setToolTip(tr("The item the queue advances to."));
    } else {
        headSource_->setToolTip(tr(
            "The item handing over. More specific rules win: an exact source beats Any."));
        headTarget_->setToolTip(tr(
            "The item taken over. More specific rules win: an exact target beats Any."));
    }
}

void TransitionEditorWidget::applyDefaultCollapse(bool force)
{
    if (collapseDirty_ && !force)
        return;
    applyingCollapse_ = true;
    const bool wide = !isNarrow();
    sourceExitSection_->setExpanded(true);
    targetEntrySection_->setExpanded(true);
    bridgeSection_->setExpanded(wide);
    resolutionSection_->setExpanded(wide);
    applyingCollapse_ = false;
    if (force)
        collapseDirty_ = false;
}

void TransitionEditorWidget::applyHostMargins()
{
    auto* root = qobject_cast<QVBoxLayout*>(layout());
    if (!root)
        return;
    const auto& inspector = ThemeManager::instance().metrics().inspector;
    root->setContentsMargins(inspector.pageMargin, inspector.pageMargin,
                             inspector.pageMargin, inspector.pageMargin);
}

void TransitionEditorWidget::refreshChrome()
{
    applyHostMargins();
}

QString TransitionEditorWidget::mountBadgeTooltip() const
{
    switch (mountState_) {
        case TransitionMountState::Editable:
            return tr("This hand-over table is active for the current mode.");
        case TransitionMountState::Idle:
            return tr("Authored, but not consumed in this mode.");
        case TransitionMountState::None:
        default:
            return tr("Not consumed in this mode.");
    }
}

QString TransitionEditorWidget::mountNoteText() const
{
    switch (mountState_) {
        case TransitionMountState::Idle:
            return tr(
                "The winning row is frozen at trigger time, so this table is authored "
                "but not consumed.");
        case TransitionMountState::None:
            return tr(
                "This mode produces no hand-over events, so the table is not consumed. "
                "It is kept and stays editable.");
        case TransitionMountState::Editable:
        default:
            return QString();
    }
}

void TransitionEditorWidget::refreshStatusBar()
{
    QString roleText;
    QString badgeText;
    switch (mountState_) {
        case TransitionMountState::Editable:
            roleText = QStringLiteral("accent");
            badgeText = tr("Editable");
            break;
        case TransitionMountState::Idle:
            roleText = QStringLiteral("warning");
            badgeText = tr("Idle");
            break;
        case TransitionMountState::None:
        default:
            roleText = QStringLiteral("muted");
            badgeText = tr("None");
            break;
    }
    mountBadge_->setText(QString(QChar(0x25CF)) + QStringLiteral(" ") + badgeText);
    QtControlSkin::setSemanticProperty(mountBadge_, "textRole", roleText);
    mountBadge_->setToolTip(mountBadgeTooltip());

    const QString mode = modeText();
    modeLabel_->setText(mode);
    modeLabel_->setToolTip(mode);
    modeLabel_->setVisible(!mode.isEmpty());
    statusSeparator_->setVisible(modeLabel_->isVisibleTo(this));

    refreshHeadTooltips();
    refreshChrome();
}

void TransitionEditorWidget::rebuildDetail()
{
    const int displayIndex = selectedDisplayIndex_;
    const TransitionRuleDef* rule = displayIndex >= 0 ? ruleAt(displayIndex) : nullptr;

    sourceExitSection_->setVisible(rule != nullptr);
    bridgeSection_->setVisible(rule != nullptr);
    targetEntrySection_->setVisible(rule != nullptr);
    resolutionSection_->setVisible(rule != nullptr);

    if (!rule) {
        QSignalBlocker sourceBlocker(sourceRef_);
        QSignalBlocker targetBlocker(targetRef_);
        QSignalBlocker bridgeBlocker(bridgeRef_);
        sourceRef_->setCurrentObjectID(0);
        targetRef_->setCurrentObjectID(0);
        bridgeRef_->setCurrentObjectID(0);
        sourceAnyLabel_->setVisible(false);
        targetAnyLabel_->setVisible(false);
        sourceRef_->setVisible(true);
        targetRef_->setVisible(true);
        bridgeSlotAdd_->setVisible(true);
        bridgeForm_->setVisible(false);
        positionRow_->setVisible(false);
        exitNoteLabel_->setVisible(false);
        refreshFooterGating();
        return;
    }

    // Every availability and visibility decision below is a projection of
    // TransitionFieldGating::gatingFor() - no mode/clock branching lives here.
    const TransitionWatchAvailability watch = TransitionSemantics::watchAvailabilityFor(
        *boundItem_, rule->source, placementContext(), itemResolver());
    const TransitionFieldGating gating =
        TransitionSemantics::gatingFor(*boundItem_, *rule, watch);

    sourceRef_->setVisible(gating.sourceEndpointEditable);
    sourceAnyLabel_->setVisible(!gating.sourceEndpointEditable);
    targetRef_->setVisible(gating.targetEndpointEditable);
    targetAnyLabel_->setVisible(!gating.targetEndpointEditable);

    ProjectItem* sourceItem = rule->source
        ? ProjectManager::instance().findItemByGuid(*rule->source) : nullptr;
    ProjectItem* targetItem = rule->target
        ? ProjectManager::instance().findItemByGuid(*rule->target) : nullptr;

    if (gating.sourceEndpointEditable) {
        QSignalBlocker blocker(sourceRef_);
        if (rule->source && !sourceItem)
            sourceRef_->setUnresolvedReference(rule->source->toString(QUuid::WithoutBraces));
        else
            sourceRef_->setCurrentObjectID(sourceItem ? sourceItem->objectID : 0);
    }
    if (gating.targetEndpointEditable) {
        QSignalBlocker blocker(targetRef_);
        if (rule->target && !targetItem)
            targetRef_->setUnresolvedReference(rule->target->toString(QUuid::WithoutBraces));
        else
            targetRef_->setCurrentObjectID(targetItem ? targetItem->objectID : 0);
    }

    if (auto* exitModel = qobject_cast<QStandardItemModel*>(exitTimingCombo_->model())) {
        auto applyExitAvail = [&](TransitionExitTiming timing, FieldAvail avail,
                                  const QString& reason) {
            const int row = exitTimingCombo_->findData(static_cast<int>(timing));
            auto* item = row >= 0 ? exitModel->item(row) : nullptr;
            if (!item) return;
            const bool enabled = avail != FieldAvail::Unavailable;
            item->setEnabled(enabled);
            item->setData(enabled ? QString() : reason, Qt::ToolTipRole);
        };
        applyExitAvail(TransitionExitTiming::Immediate, gating.exit.immediate, QString());
        applyExitAvail(TransitionExitTiming::NaturalEnd, gating.exit.naturalEnd, QString());
        applyExitAvail(TransitionExitTiming::NextMarker, gating.exit.nextMarker, gating.exit.reasonNextMarker);
        applyExitAvail(TransitionExitTiming::ExitMarker, gating.exit.exitMarker, QString());
    }
    {
        QSignalBlocker blocker(exitTimingCombo_);
        const int comboIndex =
            exitTimingCombo_->findData(static_cast<int>(rule->exitTiming));
        exitTimingCombo_->setPlaceholderText(tr("Unsupported legacy timing"));
        exitTimingCombo_->setCurrentIndex(comboIndex);
    }
    sourceFadeButton_->setFade(rule->sourceFadeOut);

    exitNoteLabel_->setText(gating.exit.note);
    exitNoteLabel_->setVisible(!gating.exit.note.isEmpty());
    if (!gating.exit.note.isEmpty()) {
        QtControlSkin::setSemanticProperty(exitNoteLabel_, "textRole",
            gating.exit.noteIsWarning ? QStringLiteral("warning") : QStringLiteral("muted"));
    }

    const bool showBridgeForm =
        rule->bridge.has_value() || (bridgeFormRevealed_ && !rule->isDefault());
    bridgeSlotAdd_->setVisible(!showBridgeForm);
    bridgeForm_->setVisible(showBridgeForm);
    if (showBridgeForm) {
        ProjectItem* bridgeItem = rule->bridge
            ? ProjectManager::instance().findItemByGuid(*rule->bridge) : nullptr;
        QSignalBlocker blocker(bridgeRef_);
        if (rule->bridge && !bridgeItem)
            bridgeRef_->setUnresolvedReference(rule->bridge->toString(QUuid::WithoutBraces));
        else
            bridgeRef_->setCurrentObjectID(bridgeItem ? bridgeItem->objectID : 0);
        bridgeFadeInButton_->setFade(rule->bridgeFadeIn);
        bridgeFadeOutButton_->setFade(rule->bridgeFadeOut);
        for (TransitionFadeButton* slot : {bridgeFadeInButton_, bridgeFadeOutButton_}) {
            slot->setEnabled(gating.bridgeFadesLive);
            slot->setDimmed(!gating.bridgeFadesLive);
            if (!gating.bridgeFadesLive)
                slot->setToolTip(gating.reasonBridgeFades);
        }
    }

    const bool bridgeAvailable = gating.bridge != FieldAvail::Unavailable;
    bridgeInsertButton_->setEnabled(bridgeAvailable);
    bridgeInsertButton_->setToolTip(bridgeAvailable ? QString() : gating.reasonBridge);
    bridgeRef_->setSelectorEnabled(bridgeAvailable);

    {
        QSignalBlocker blocker(entryModeCombo_);
        if (auto* model = qobject_cast<QStandardItemModel*>(entryModeCombo_->model())) {
            const int samePositionRow = entryModeCombo_->findData(
                static_cast<int>(TransitionEntryMode::SamePosition));
            if (auto* samePositionItem = model->item(samePositionRow)) {
                const bool enabled = gating.samePosition != FieldAvail::Unavailable;
                samePositionItem->setEnabled(enabled);
                samePositionItem->setData(enabled ? QString() : gating.reasonSamePosition,
                                          Qt::ToolTipRole);
            }
        }
        const int comboIndex =
            entryModeCombo_->findData(static_cast<int>(rule->entryMode));
        entryModeCombo_->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
    }
    targetFadeInButton_->setFade(rule->targetFadeIn);

    positionRow_->setVisible(gating.samePositionReadoutVisible);
    if (gating.samePositionReadoutVisible)
        positionValue_->setText(samePositionReadoutText(displayIndex));

    refreshFooterGating();
}

void TransitionEditorWidget::rejectInteraction(const QString& error)
{
    lastInteractionError_ = error.isEmpty() ? tr("Transition edit was rejected.") : error;
    ToastManager::instance().showToast(lastInteractionError_, 2500, ToastType::Warning);
}

bool TransitionEditorWidget::commitTable(TransitionTableDef candidate, const QString& undoText)
{
    if (!boundItem_)
        return false;
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
            {QStringLiteral("property"), QStringLiteral("transitions")},
            {QStringLiteral("value"), candidate.toJson()},
            {QStringLiteral("undo_text"), undoText}
        },
        ActionSource::GUI, this);
    if (!result.success) {
        rejectInteraction(result.errorMessage);
        return false;
    }
    lastInteractionError_.clear();
    rebuildRuleList();
    refreshRehearsalWinner();
    refreshWatchView();
    return true;
}

QString TransitionEditorWidget::endpointName(const std::optional<QUuid>& guid) const
{
    if (!guid)
        return tr("Any");
    ProjectItem* item = ProjectManager::instance().findItemByGuid(*guid);
    return item ? item->name : tr("(missing)");
}

QLabel* TransitionEditorWidget::makeEndpointCell(QWidget* parent,
                                                 const std::optional<QUuid>& guid) const
{
    if (!guid)
        return makeCellLabel(parent, tr("Any"), QStringLiteral("secondary"));
    ProjectItem* item = ProjectManager::instance().findItemByGuid(*guid);
    if (item)
        return makeCellLabel(parent, item->name, QStringLiteral("primary"));
    QLabel* cell = makeCellLabel(parent, guid->toString(QUuid::WithoutBraces).left(8),
                                 QStringLiteral("error"));
    cell->setToolTip(tr("Unresolved reference: %1")
                         .arg(guid->toString(QUuid::WithoutBraces)));
    return cell;
}

QString TransitionEditorWidget::exitTimingText(TransitionExitTiming timing) const
{
    switch (timing) {
        case TransitionExitTiming::Immediate:  return tr("Immediate");
        case TransitionExitTiming::NaturalEnd: return tr("Natural End");
        case TransitionExitTiming::NextBeat:
        case TransitionExitTiming::NextBar:    return tr("Unsupported legacy timing");
        case TransitionExitTiming::NextMarker:    return tr("Next Marker");
        case TransitionExitTiming::ExitMarker:    return tr("Exit Marker");
    }
    return QString();
}

QString TransitionEditorWidget::entryModeText(TransitionEntryMode mode) const
{
    switch (mode) {
        case TransitionEntryMode::EntryMarker:     return tr("Entry Marker");
        case TransitionEntryMode::SamePosition: return tr("Same Position");
    }
    return QString();
}

QString TransitionEditorWidget::mountStateExplanation() const
{
    switch (mountState_) {
        case TransitionMountState::Editable:
            return tr("Editable - this hand-over table is active for the current mode.");
        case TransitionMountState::Idle:
            return tr("Idle - this hand-over table is authored but not consumed by the "
                      "current evaluation.");
        case TransitionMountState::None:
        default:
            return tr("None - this mode does not produce hand-over events.");
    }
}

QString TransitionEditorWidget::modeText() const
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    if (!selection)
        return QString();
    switch (selection->mode) {
        case SelectionMode::Random: return selection->isQueue() ? tr("Random / Queue") : tr("Random");
        case SelectionMode::Sequence: return selection->isQueue() ? tr("Sequence / Queue") : tr("Sequence");
        case SelectionMode::Match: return tr("Match");
        case SelectionMode::Blend: return tr("Blend");
    }
    return QString();
}

TimelineItemResolver TransitionEditorWidget::itemResolver() const
{
    return [](const QUuid& guid) { return ProjectManager::instance().findItemByGuid(guid); };
}

std::optional<QUuid> TransitionEditorWidget::ruleBridge(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? rule->bridge : std::nullopt;
}

TransitionFadeDef TransitionEditorWidget::ruleFade(int displayIndex, TransitionFadeSlot slot) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    return rule ? *fadeSlotPtr(*rule, slot) : TransitionFadeDef();
}

bool TransitionEditorWidget::applyRuleUpdate(int displayIndex, TransitionRuleDef rule,
                                            const QString& undoText)
{
    const auto* current = table();
    if (!current) {
        rejectInteraction(tr("No transition table is bound."));
        return false;
    }
    const std::vector<int> order = TransitionSemantics::rulesForDisplay(*current);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(order.size())) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionTableDef candidate = *current;
    const TransitionValidation validation = candidate.updateRule(order[displayIndex], rule);
    if (!validation) {
        rejectInteraction(validation.message);
        rebuildDetail();
        return false;
    }
    selectedEndpoints_ = Endpoints{rule.source, rule.target};
    return commitTable(std::move(candidate), undoText);
}

bool TransitionEditorWidget::setRuleEndpoints(int displayIndex, const std::optional<QUuid>& source,
                                             const std::optional<QUuid>& target)
{
    const TransitionRuleDef* current = ruleAt(displayIndex);
    if (!current) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    for (const auto& endpoint : {source, target}) {
        if (!endpoint) continue;
        ProjectItem* item = ProjectManager::instance().findItemByGuid(*endpoint);
        if (item && (!boundItem_
                     || !TransitionSemantics::isEndpointCandidate(*boundItem_, *item))) {
            rejectInteraction(tr("Transition endpoints must be direct children of this Container."));
            return false;
        }
    }
    TransitionRuleDef rule = *current;
    rule.source = source;
    rule.target = target;
    return applyRuleUpdate(displayIndex, std::move(rule), tr("Set Transition Endpoints"));
}

bool TransitionEditorWidget::setRuleExitTiming(int displayIndex, TransitionExitTiming timing)
{
    const TransitionRuleDef* current = ruleAt(displayIndex);
    if (!current) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionRuleDef rule = *current;
    rule.exitTiming = timing;
    return applyRuleUpdate(displayIndex, std::move(rule), tr("Set Transition Exit Timing"));
}

bool TransitionEditorWidget::setRuleBridge(int displayIndex, const std::optional<QUuid>& bridge)
{
    const TransitionRuleDef* current = ruleAt(displayIndex);
    if (!current) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionRuleDef rule = *current;
    rule.bridge = bridge;
    return applyRuleUpdate(displayIndex, std::move(rule), tr("Set Transition Bridge"));
}

bool TransitionEditorWidget::setRuleEntryMode(int displayIndex, TransitionEntryMode mode)
{
    const TransitionRuleDef* current = ruleAt(displayIndex);
    if (!current) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionRuleDef rule = *current;
    rule.entryMode = mode;
    return applyRuleUpdate(displayIndex, std::move(rule), tr("Set Transition Entry Mode"));
}

bool TransitionEditorWidget::setRuleFade(int displayIndex, TransitionFadeSlot slot,
                                        const TransitionFadeDef& fade)
{
    const TransitionRuleDef* current = ruleAt(displayIndex);
    if (!current) {
        rejectInteraction(tr("Transition rule does not exist."));
        return false;
    }
    TransitionRuleDef rule = *current;
    *fadeSlotPtr(rule, slot) = fade;
    return applyRuleUpdate(displayIndex, std::move(rule), tr("Set Transition Fade"));
}

void TransitionEditorWidget::refreshRehearsalWinner()
{
    if (!boundItem_)
        return;
    // A rehearsal winner only means something when the mode actually
    // evaluates one (queue advance or grid re-evaluation); None modes never
    // produce a winner, so skip recomputing it. The read-out itself still
    // needs to reflect the current (possibly stale-from-before-the-mode-
    // switch) winner, so it is refreshed unconditionally below.
    if (mountState_ != TransitionMountState::None) {
        const auto* selection = boundItem_->comp<VSelection>();
        if (selection) {
            const std::optional<QUuid> winner = TransitionRehearsal::winnerForContainer(
                *boundItem_, ProjectManager::instance().variableRehearsal(), itemResolver(),
                currentWorldPlatformScope(PlatformContextService::instance().context()));
            ProjectManager::instance().transitionRehearsal().refreshWinner(boundItem_->guid, winner);
        }
    }
    handoverValue_->setText(handoverReadoutText());
    handoverValue_->setToolTip(isQueueMode()
        ? tr("Illustrative only - the real queue order is not predicted here.")
        : QString());
}

bool TransitionEditorWidget::spliceReadoutVisible() const
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    return isQueueMode() && selection && selection->spliceByMarker;
}

void TransitionEditorWidget::refreshSpliceReadout()
{
    spliceRow_->setVisible(spliceReadoutVisible());
}

void TransitionEditorWidget::refreshWatchView()
{
    TransitionSemantics::TransitionWatchView nextView;
    if (boundItem_ && mountState_ != TransitionMountState::None) {
        const std::optional<QUuid> winner =
            ProjectManager::instance().transitionRehearsal().currentWinner(boundItem_->guid);
        nextView = TransitionSemantics::buildTransitionWatchView(
            *boundItem_, winner, placementContext(), itemResolver());
    }

    if (watchView_ == nextView)
        return;
    watchView_ = std::move(nextView);
    if (watchView_.containerGuid.isNull()) {
        watchValue_->clear();
        watchValue_->setToolTip(QString());
    } else {
        watchValue_->setText(watchReadoutText());
        watchValue_->setToolTip(watchReadoutTooltip());
    }
    emit watchViewChanged(watchView_);
}

QString TransitionEditorWidget::watchReadoutText() const
{
    if (!watchView_.hasSource)
        return tr("[none]  no source");
    if (!watchView_.found())
        return tr("[none]  no reference");

    QStringList names;
    for (const QUuid& guid : watchView_.path)
        names << endpointName(guid);
    const ProjectItem* reference = watchView_.referencePiece
        ? ProjectManager::instance().findItemByGuid(*watchView_.referencePiece) : nullptr;
    if (reference && !AuthoringPolicy::validateTimeline(*reference, itemResolver()))
        return tr("[%1]  %2  (unsupported arrangement)")
            .arg(watchDirectionTag(watchView_.direction), names.join(QStringLiteral(" -> ")));
    QString text = tr("[%1]  %2  (marker reference)")
        .arg(watchDirectionTag(watchView_.direction), names.join(QStringLiteral(" -> ")));

    if (watchView_.direction != TransitionSemantics::TransitionWatchDirection::Down) {
        if (!watchView_.placementContextProvided)
            text += tr("  · org. scope only");
        else if (!watchView_.placementContextValid)
            text += tr("  · placement invalid");
    }
    return text;
}

QString TransitionEditorWidget::watchReadoutTooltip() const
{
    if (!watchView_.hasSource || !watchView_.found())
        return watchView_.failureReason;

    const ProjectItem* reference = watchView_.referencePiece
        ? ProjectManager::instance().findItemByGuid(*watchView_.referencePiece) : nullptr;
    if (reference) {
        const auto authoring = AuthoringPolicy::validateTimeline(*reference, itemResolver());
        if (!authoring) return authoring.message;
    }

    if (watchView_.direction != TransitionSemantics::TransitionWatchDirection::Down) {
        if (!watchView_.placementContextProvided)
            return tr("No placement context; organization scope only.");
        if (!watchView_.placementContextValid) {
            return tr("Placement context invalid: %1; fell back to organization scope.")
                .arg(watchView_.failureReason);
        }
    }
    return watchView_.failureReason;
}

void TransitionEditorWidget::openFadePopup(TransitionFadeSlot slot, QWidget* anchor)
{
    if (selectedDisplayIndex_ < 0 || !anchor)
        return;
    const TransitionRuleDef* rule = ruleAt(selectedDisplayIndex_);
    if (!rule)
        return;

    // Resolve the target rule by endpoint identity rather than by the display
    // index captured at popup-open time: an undo/redo triggered while the
    // (still open) popup has focus can reorder the specificity-sorted table,
    // which would otherwise commit the edit to whatever now sits at the old
    // index.
    const std::optional<QUuid> capturedSource = rule->source;
    const std::optional<QUuid> capturedTarget = rule->target;
    auto resolveCurrentIndex = [this, capturedSource, capturedTarget]() -> int {
        const auto* t = table();
        if (!t)
            return -1;
        const std::vector<int> order = TransitionSemantics::rulesForDisplay(*t);
        for (size_t display = 0; display < order.size(); ++display) {
            const TransitionRuleDef& candidate = t->rules[order[display]];
            if (candidate.source == capturedSource && candidate.target == capturedTarget)
                return static_cast<int>(display);
        }
        return -1;
    };

    QString title;
    std::optional<QUuid> contextGuid;
    switch (slot) {
        case TransitionFadeSlot::SourceFadeOut:
            title = tr("Source Fade Out");
            contextGuid = rule->source;
            break;
        case TransitionFadeSlot::BridgeFadeIn:
            title = tr("Bridge Fade In");
            contextGuid = rule->bridge;
            break;
        case TransitionFadeSlot::BridgeFadeOut:
            title = tr("Bridge Fade Out");
            contextGuid = rule->bridge;
            break;
        case TransitionFadeSlot::TargetFadeIn:
            title = tr("Target Fade In");
            contextGuid = rule->target;
            break;
    }

    TransitionFadePopupContext context;
    context.objectName = endpointName(contextGuid);
    if (contextGuid) {
        if (ProjectItem* item = ProjectManager::instance().findItemByGuid(*contextGuid))
            context.objectIcon = iconForItem(*item);
    } else {
        context.dimmed = true;
    }

    auto* popup = new TransitionFadePopup(title, context, *fadeSlotPtr(*rule, slot), this,
        [this, resolveCurrentIndex, slot](const TransitionFadeDef& fade) {
            const int currentIndex = resolveCurrentIndex();
            if (currentIndex >= 0)
                setRuleFade(currentIndex, slot, fade);
        },
        [this, resolveCurrentIndex, slot]() {
            const int currentIndex = resolveCurrentIndex();
            const TransitionRuleDef* current = currentIndex >= 0 ? ruleAt(currentIndex) : nullptr;
            return current ? *fadeSlotPtr(*current, slot) : TransitionFadeDef();
        });
    popup->showAt(anchor->mapToGlobal(anchor->rect().bottomLeft()));
}

QString TransitionEditorWidget::samePositionReadoutText(int displayIndex) const
{
    const TransitionRuleDef* rule = ruleAt(displayIndex);
    if (!rule)
        return tr("Unknown: rule does not exist.");
    if (!rule->source)
        return tr("Unknown: rule has no explicit source.");
    if (!rule->target)
        return tr("Unknown: rule has no explicit target.");

    ProjectItem* source = ProjectManager::instance().findItemByGuid(*rule->source);
    if (!source)
        return tr("Unknown: source is missing.");
    ProjectItem* target = ProjectManager::instance().findItemByGuid(*rule->target);
    if (!target)
        return tr("Unknown: target is missing.");

    const TimelineItemResolver resolve = itemResolver();
    if (!AuthoringPolicy::validateTimeline(*source, resolve)
        || !AuthoringPolicy::validateTimeline(*target, resolve))
        return tr("Unsupported arrangement. Position is unavailable for this timeline.");
    const TimelineSemanticsResult sourceSpan =
        TimelineSemantics::nominalSpanOf(*source, resolve);
    if (!sourceSpan)
        return tr("Unknown: %1").arg(sourceSpan.validation.message);

    const TransitionSemantics::SamePositionResult mapped = TransitionSemantics::samePositionMap(
        sourceSpan.nominal.end, *source, *target, resolve);
    if (!mapped)
        return tr("Unknown: %1").arg(mapped.validation.message);

    return tr("%1 -> %2 in %3")
        .arg(coordinateText(sourceSpan.nominal.end, sourceSpan.time),
             coordinateText(mapped.targetPosition, mapped.targetTime),
             target->name);
}

QString TransitionEditorWidget::handoverReadoutText() const
{
    if (!boundItem_)
        return tr("No hand-over rehearsal available.");
    const auto* t = table();
    if (!t)
        return tr("No hand-over rehearsal available.");

    const TransitionSemantics::HandoverResult handover =
        ProjectManager::instance().transitionRehearsal().handoverFor(boundItem_->guid, *t);

    QString text;
    switch (handover.kind) {
        case TransitionSemantics::HandoverKind::Transition: {
            QStringList names;
            for (const QUuid& guid : handover.referenceChain)
                names << endpointName(guid);
            text = names.join(QStringLiteral(" -> "));
            break;
        }
        case TransitionSemantics::HandoverKind::SourceExit:
            text = tr("source exiting with no successor");
            break;
        case TransitionSemantics::HandoverKind::SilentEntry:
            text = tr("entering silently with no predecessor");
            break;
        case TransitionSemantics::HandoverKind::SameObject:
            text = tr("same object continues playing");
            break;
        case TransitionSemantics::HandoverKind::NoMatch:
        default:
            text = tr("no rehearsal winner yet");
            break;
    }
    return isQueueMode() ? tr("%1  (illustrative)").arg(text) : text;
}

bool TransitionEditorWidget::tableReferences(const QUuid& guid) const
{
    const TransitionTableDef* t = table();
    if (!t || guid.isNull())
        return false;
    return std::any_of(t->rules.begin(), t->rules.end(),
        [&guid](const TransitionRuleDef& rule) {
            return rule.source == guid || rule.target == guid || rule.bridge == guid;
        });
}

bool TransitionEditorWidget::isQueueMode() const
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    return selection && selection->isQueue();
}

void TransitionEditorWidget::showState(const QString& text)
{
    stateLabel_->setText(text);
    stack_->setCurrentWidget(stateLabel_);
    addButton_->setEnabled(false);
    removeButton_->setEnabled(false);
}

}
