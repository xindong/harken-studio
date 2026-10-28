
#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ActionManager.h"
#include "Core/AppSettings.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"
#include "Core/UndoCommands.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Core/AssetPipeline/PlatformProfile.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/AudioEngine.h"
#include "Core/FontManager.h"
#include "Widgets/Common/ReferenceImpactDialog.h"
#include "Widgets/Common/SearchField.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHeaderView>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QInputDialog>
#include <QApplication>
#include <cmath>

#include <QUrl>
#include <QFileInfo>
#include <QJsonValue>
#include <QDir>
#include <QDirIterator>
#include <QFocusEvent>
#include <QChildEvent>
#include <QVector>
#include <optional>

#include "Core/Components/VAudio.h"
#include "Core/Components/VBus.h"

namespace VNX {

namespace {

inline ProjectItem* findItemByObjectId(ProjectData* project, uint32_t objectId)
{
    if (!project || objectId == 0) return nullptr;
    ProjectItem* hit = nullptr;
    std::function<void(ProjectItem*)> walk = [&](ProjectItem* pi) {
        if (!pi || hit) return;
        if (pi->objectID == objectId) { hit = pi; return; }
        for (auto& c : pi->children) walk(c.get());
    };
    for (auto* wu : project->allFileBindings())
        if (wu && wu->rootItem && !hit) walk(wu->rootItem.get());
    return hit;
}

QString variableRangeConflictDetails(const QJsonArray& references)
{
    QStringList lines;
    for (int i = 0; i < references.size(); ++i) {
        const QJsonObject reference = references[i].toObject();
        lines.append(QObject::tr(
            "%1. %2 — row %3 (%4), column %5, value %6")
                         .arg(i + 1)
                         .arg(reference.value(QStringLiteral("grid_name")).toString())
                         .arg(reference.value(QStringLiteral("row")).toInt())
                         .arg(reference.value(QStringLiteral("row_name")).toString())
                         .arg(reference.value(QStringLiteral("column")).toInt())
                         .arg(reference.value(QStringLiteral("value")).toInt()));
    }
    return lines.join(QChar('\n'));
}

}

InspectorPanelPrivate::InspectorPanelPrivate(InspectorPanel* q)
    : q_ptr(q)
{
}

InspectorPanelPrivate::~InspectorPanelPrivate() = default;

InspectorPanel::InspectorPanel(QWidget* parent)
    : QWidget(parent)
{
    d = std::make_unique<InspectorPanelPrivate>(this);
    setObjectName("InspectorPanel");
    d->setupUI();

    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged,
            this, &InspectorPanel::onFontSettingsChanged);

    int fontSize = AppSettings::instance().panelFontSize(PanelId::Inspector);
    QFont font = FontManager::instance().getFont(FontRole::Body, fontSize);
    setFont(font);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &InspectorPanel::applyTheme);

    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
            this, [this](ProjectItem* item, const QString& prop, const QObject* origin) {
        if (!item) return;
        if (origin && ownsEditorOrigin(origin))
            return;
        d->refreshBoundPages(item, prop);
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyPreviewed,
            this, [this](ProjectItem* item, const QString& prop, const QObject* origin) {
        if (!item || (origin && ownsEditorOrigin(origin))) return;
        d->refreshBoundPages(item, prop);
    });
    connect(&ProjectManager::instance(), &ProjectManager::busChanged,
            this, [this](uint32_t busID, const QString& prop, const QObject* origin) {
        if ((prop != QLatin1String("sends") && prop != QLatin1String("send")
             && prop != QLatin1String("outputBusVolume"))
            || (origin && ownsEditorOrigin(origin))) return;
        if (auto* item = ProjectManager::instance().findBusItem(busID))
            d->refreshBoundPages(item, prop == QLatin1String("send") ? QStringLiteral("sends") : prop);
    });

    connect(&ProjectManager::instance(), &ProjectManager::itemMoved,
            this, [this](ProjectItem* , ProjectItem* , ProjectItem* ) {
        d->rebindHierarchyPages(nullptr);
    });

    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
            this, [this](ProjectItem* item) {
        if (item && d->currentContextContains(item)) {
            showEmpty();
            return;
        }
        d->rebindHierarchyPages(nullptr);
    });

    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, &InspectorPanel::showEmpty);

    connect(&ProjectManager::instance(), &ProjectManager::targetPlatformsChanged,
            this, [this](QStringList) {
        ProjectItem* current = currentContextItem();
        const std::optional<PlacementContext> placement = placementContext_;
        d->invalidateBuiltPages();
        if (!current)
            return;
        if (placement)
            showItemInspector(current, *placement);
        else
            showItemInspector(current);
    });

    connect(&ProjectManager::instance(), &ProjectManager::itemCreated,
            this, [this](ProjectItem* item) {
        if (item)
            d->rebindHierarchyPages(item);
    });

    setFocusPolicy(Qt::StrongFocus);
    installFocusFilterRecursively(this);
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget* previous, QWidget* current) {
        if (!current && previous && previous != this && isAncestorOf(previous)
            && isVisible() && isEnabled()
            && QApplication::activeWindow() == window()
            && !QApplication::activePopupWidget())
            setFocus(Qt::OtherFocusReason);
    });
}

InspectorPanel::~InspectorPanel()
{
    QObject::disconnect(qApp, nullptr, this, nullptr);
}

void InspectorPanelPrivate::refreshBoundPages(ProjectItem* item, const QString& prop)
{
    if (busEngine_ && currentBusItem_) {
        if (prop == QLatin1String("fieldGroups") || prop == QLatin1String("outputBusId")
            || prop == QLatin1String("outputBusVolume"))
            busEngine_->refreshField(QStringLiteral("inputs"));
        if (prop == QLatin1String("fieldGroups") || prop == QLatin1String("sends")
            || prop == QLatin1String("send"))
            busEngine_->refreshField(QStringLiteral("receives"));
    }
    const auto refreshOrRebind = [&](PropertyEditorEngine* engine, ProjectItem* current) {
        if (!engine || !current) return;
        bool related = item == current;
        if (!related)
            related = engine->playablePropertyEventRelated(item, prop);
        if (!related && (prop == QLatin1String("bindings")
                         || prop.startsWith(QLatin1String("bindings.")))) {
            for (ProjectItem* ancestor = current->parent; ancestor; ancestor = ancestor->parent)
                if (ancestor == item) {
                    related = true;
                    break;
                }
        }
        if (!related && item->type == ProjectItem::Type::Variable)
            related = true;
        if (!related && current->type == ProjectItem::Type::Segment
            && (prop == QLatin1String("clips")
                || prop == QLatin1String("markers")
                || prop == QLatin1String("segmentTime")))
            related = true;
        if (!related) return;
        if (engine->refreshField(prop))
            return;
        const bool bindingProp = prop == QLatin1String("bindings")
            || prop.startsWith(QLatin1String("bindings."));
        const bool ancestorOrVariable =
            item != current
            && (item->type == ProjectItem::Type::Variable || bindingProp);
        if (!ancestorOrVariable && !bindingProp)
            engine->bindItem(current);
    };
    refreshOrRebind(soundEngine_,     currentSoundItem_);
    refreshOrRebind(layerEngine_,     currentLayerItem_);
    refreshOrRebind(containerEngine_, currentContainerItem_);
    refreshOrRebind(trackEngine_,     currentTrackItem_);
    refreshOrRebind(segmentEngine_,     currentSegmentItem_);
    refreshOrRebind(busEngine_,       currentBusItem_);
    refreshOrRebind(variableEngine_,  currentVariableItem_);
    refreshOrRebind(actionEngine_,    currentActionItem_);
    refreshOrRebind(attenuationEngine_, currentAttenuationItem_);
}

void InspectorPanelPrivate::rebindHierarchyPages(ProjectItem* createdChild)
{
    const auto rebind = [&](PropertyEditorEngine* engine, ProjectItem* current) {
        if (!engine || !current) return;
        if (createdChild && createdChild->parent != current
            && current->type != ProjectItem::Type::Bus) return;
        engine->bindItem(current);
    };
    rebind(soundEngine_,     currentSoundItem_);
    rebind(layerEngine_,     currentLayerItem_);
    rebind(containerEngine_, currentContainerItem_);
    rebind(trackEngine_,     currentTrackItem_);
    rebind(segmentEngine_,     currentSegmentItem_);
    rebind(busEngine_,       currentBusItem_);
    rebind(variableEngine_,  currentVariableItem_);
}

void InspectorPanelPrivate::clearItemBindings()
{
    const std::array<PropertyEditorEngine*, 9> engines = {
        soundEngine_, layerEngine_, containerEngine_, trackEngine_, segmentEngine_,
        variableEngine_, busEngine_, actionEngine_, attenuationEngine_};
    for (PropertyEditorEngine* engine : engines)
        if (engine) engine->bindItem(nullptr);

    currentSoundItem_ = nullptr;
    currentLayerItem_ = nullptr;
    currentContainerItem_ = nullptr;
    currentTrackItem_ = nullptr;
    currentSegmentItem_ = nullptr;
    currentVariableItem_ = nullptr;
    currentBusItem_ = nullptr;
    currentActionItem_ = nullptr;
    currentAttenuationItem_ = nullptr;
}

bool InspectorPanelPrivate::currentContextContains(ProjectItem* item) const
{
    if (!item) return false;
    ProjectItem* current = q_ptr->currentContextItem();
    if (!current) return false;
    for (ProjectItem* ancestor = current; ancestor; ancestor = ancestor->parent)
        if (ancestor == item) return true;
    return false;
}

void InspectorPanelPrivate::wireAuthoringCommit(PropertyEditorEngine* engine)
{
    QObject::connect(engine, &PropertyEditorEngine::fieldGroupChanged,
                     q_ptr, &InspectorPanel::propertyChanged);
    QObject::connect(engine, &PropertyEditorEngine::fieldChanged, q_ptr,
        [this, engine](const QString& field, const QVariant&, const QVariant& newVal) {
            ProjectItem* item = engine->currentItem();
            if (!item) return;
            const auto& context = PlatformContextService::instance().context();
            QJsonObject actionArgs = engine->buildSetItemPropertyArgs(
                field, newVal,
                context.all || context.selected.isEmpty()
                    ? std::nullopt
                    : std::optional<QVector<ProjectPlatformId>>(
                          PlatformContextService::instance().context().selected));
            ActionResult result = ActionManager::instance().execute(
                "set_item_property", actionArgs, ActionSource::Internal, engine);
            if (!result.success
                && result.errorCode == QLatin1String("VARIABLE_RANGE_CONFLICT")) {
                const QJsonArray references = result.data.value(
                    QStringLiteral("invalid_references")).toArray();
                const QString summary = result.errorMessage
                    + QStringLiteral("\n\n")
                    + QObject::tr(
                        "Changing the Range anyway will keep these values, but "
                        "they cannot match until the Grid or Variable is updated.");
                if (confirmReferenceImpact(
                        q_ptr, QObject::tr("Variable Range Affects Grid Values"), summary,
                        variableRangeConflictDetails(references),
                        QObject::tr("Change Anyway"), false,
                        QStringLiteral("variableRangeImpactDialog"))) {
                    actionArgs[QStringLiteral("allow_invalid_references")] = true;
                    result = ActionManager::instance().execute(
                        "set_item_property", actionArgs,
                        ActionSource::Internal, engine);
                }
            }
            if (!result.success || field == QLatin1String("outputBusId")
                || field == QLatin1String("range")
                || PlayablePropertyAuthoringV1::fieldByName(field))
                engine->refreshField(field);
            else
                engine->refreshDomains();
            emit q_ptr->propertyChanged();
        });
}

bool InspectorPanel::ownsEditorOrigin(const QObject* origin) const
{
    if (d->ownsEditorEngine(origin))
        return true;
    for (const QObject* current = origin; current; current = current->parent()) {
        if (current == this)
            return true;
    }
    return false;
}

void InspectorPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root) return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

void InspectorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

bool InspectorPanel::focusNextPrevChild(bool next)
{
    QWidget* focused = QApplication::focusWidget();
    if (focused && focused != this && isAncestorOf(focused)
        && isVisible() && isEnabled()
        && !QApplication::activePopupWidget()
        && (!focused->isVisibleTo(this) || !focused->isEnabledTo(this))) {
        setFocus(Qt::OtherFocusReason);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

bool InspectorPanel::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t == QEvent::FocusIn) {
        emit panelFocused(panelId_);
    } else if (t == QEvent::ChildAdded) {
        if (auto* ce = static_cast<QChildEvent*>(event)) {
            if (QObject* added = ce->child())
                installFocusFilterRecursively(added);
        }
    }
    return QWidget::eventFilter(watched, event);
}


void InspectorPanelPrivate::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(q_ptr);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    QWidget* searchContainer = new QWidget();
    QHBoxLayout* searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(8, 8, 8, 8);

    searchBox_ = new SearchField();
    searchBox_->setPlaceholderText(q_ptr->tr("Search properties..."));
    QObject::connect(searchBox_, &QLineEdit::textChanged, q_ptr, &InspectorPanel::onSearchTextChanged);
    searchLayout->addWidget(searchBox_);

    mainLayout->addWidget(searchContainer);

    scrollArea_ = new QScrollArea();
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    stack_ = new QStackedWidget();

    registerPages();

    scrollArea_->setWidget(stack_);
    mainLayout->addWidget(scrollArea_);

    stack_->setCurrentIndex(PageEmpty);
}

void InspectorPanelPrivate::registerPages()
{
    pages_[PageEmpty].builder         = [this]{ setupEmptyPage(); };
    pages_[PageAction].builder        = [this]{ setupActionPage(); };
    pages_[PageAttenuation].builder   = [this]{ setupAttenuationPage(); };
    pages_[PageSound].builder         = [this]{ setupSoundPageEngine(); };
    pages_[PageLayer].builder         = [this]{ setupLayerPage(); };
    pages_[PageBus].builder           = [this]{ setupBusPage(); };
    pages_[PageContainer].builder     = [this]{ setupContainerPage(); };
    pages_[PageTrack].builder         = [this]{ setupTrackPage(); };
    pages_[PageSegment].builder         = [this]{ setupSegmentPage(); };
    pages_[PageVariable].builder      = [this]{ setupVariablePage(); };

    pages_[PageEmpty].builder();
    pages_[PageEmpty].built = true;

    for (int i = PageEmpty + 1; i < PageCount; ++i)
        stack_->addWidget(new QWidget());
}

int InspectorPanelPrivate::ensurePageBuilt(Page p)
{
    PageSlot& slot = pages_[p];
    if (slot.built)
        return p;

    const int appendedAt = stack_->count();
    slot.builder();
    QWidget* real = stack_->widget(appendedAt);
    stack_->removeWidget(real);

    QWidget* placeholder = stack_->widget(p);
    stack_->removeWidget(placeholder);
    delete placeholder;
    stack_->insertWidget(p, real);

    slot.built = true;
    return p;
}

void InspectorPanelPrivate::dropEnginePageBindings()
{
    const std::array<PropertyEditorEngine*, 9> engines = {
        soundEngine_, layerEngine_, containerEngine_, trackEngine_, segmentEngine_,
        variableEngine_, busEngine_, actionEngine_, attenuationEngine_};
    for (PropertyEditorEngine* engine : engines)
        if (engine) engine->releasePage();
}

void InspectorPanelPrivate::invalidateBuiltPages()
{
    const int current = stack_->currentIndex();
    dropEnginePageBindings();
    for (int i = PageEmpty + 1; i < PageCount; ++i) {
        if (!pages_[i].built)
            continue;
        QWidget* stale = stack_->widget(i);
        stack_->removeWidget(stale);
        delete stale;
        stack_->insertWidget(i, new QWidget());
        pages_[i].built = false;
    }
    if (current >= 0 && current < stack_->count())
        stack_->setCurrentIndex(current);
}

void InspectorPanelPrivate::setupEmptyPage()
{
    stack_->addWidget(new QWidget());
}

void InspectorPanel::showEmpty()
{
    placementContext_.reset();
    d->clearItemBindings();
    d->updateContext(QString(), QString());
    d->stack_->setCurrentIndex(InspectorPanelPrivate::PageEmpty);
}

InspectorPanelPrivate::Page pageForItemType(ProjectItem::Type type)
{
    using P = InspectorPanelPrivate;
    switch (type) {
    case ProjectItem::Type::None:      return P::PageEmpty;
    case ProjectItem::Type::Sound:       return P::PageSound;
    case ProjectItem::Type::Container:   return P::PageContainer;
    case ProjectItem::Type::Track:       return P::PageTrack;
    case ProjectItem::Type::Segment:       return P::PageSegment;
    case ProjectItem::Type::Layer:       return P::PageLayer;
    case ProjectItem::Type::Bus:         return P::PageBus;
    case ProjectItem::Type::Action:      return P::PageAction;
    case ProjectItem::Type::Variable:    return P::PageVariable;
    case ProjectItem::Type::Attenuation: return P::PageAttenuation;
    }
    return P::PageEmpty;
}

void InspectorPanel::showItemInspector(ProjectItem* item)
{
    placementContext_.reset();
    showItemInspectorImpl(item);
}

void InspectorPanel::showItemInspector(ProjectItem* item,
                                       const PlacementContext& placement)
{
    placementContext_ = placement;
    showItemInspectorImpl(item);
}

void InspectorPanel::showItemInspectorImpl(ProjectItem* item)
{
    if (!item) { showEmpty(); return; }

    if (item->type == ProjectItem::Type::Bus) {
        showBusInspector(item->objectID);
        return;
    }

    d->clearItemBindings();

    using P = InspectorPanelPrivate;
    const P::Page page = pageForItemType(item->type);
    if (page == P::PageEmpty
        || !ItemTypeCatalog::isUserFacingType(item->type)) {
        showEmpty();
        return;
    }

    d->ensurePageBuilt(page);
    d->updateContext(ItemTypeCatalog::displayName(item->type), item->name);
    d->stack_->setCurrentIndex(page);

    PropertyEditorEngine* engine = nullptr;
    switch (page) {
    case P::PageAction:      d->currentActionItem_      = item; engine = d->actionEngine_;      break;
    case P::PageAttenuation: d->currentAttenuationItem_ = item; engine = d->attenuationEngine_; break;
    case P::PageSound:       d->currentSoundItem_       = item; engine = d->soundEngine_;       break;
    case P::PageLayer:       d->currentLayerItem_       = item; engine = d->layerEngine_;       break;
    case P::PageContainer:   d->currentContainerItem_   = item; engine = d->containerEngine_;   break;
    case P::PageTrack:       d->currentTrackItem_       = item; engine = d->trackEngine_;       break;
    case P::PageSegment:       d->currentSegmentItem_       = item; engine = d->segmentEngine_;       break;
    case P::PageVariable:    d->currentVariableItem_    = item; engine = d->variableEngine_;    break;
    default: break;
    }
    if (engine) {
        engine->bindItem(item);
    }

}

const PlacementContext* InspectorPanel::placementContext() const
{
    return placementContext_ ? &*placementContext_ : nullptr;
}

void InspectorPanel::showCanvasNode(uint32_t anchorId)
{
    if (anchorId == 0) { showEmpty(); return; }
    auto* project = ProjectManager::instance().currentProject();
    if (!project) { showEmpty(); return; }

    if (auto* item = findItemByObjectId(project, anchorId)) {
        showItemInspector(item);
        return;
    }

    {
        auto& pm = ProjectManager::instance();
        if (auto* busItem = pm.findBusItem(anchorId)) {
            if (auto* bc = busItem->comp<VNX::VBus>()) {
                showBusInspector(busItem->objectID);
                return;
            }
        }
    }

    showEmpty();
}

void InspectorPanelPrivate::updateContext(const QString& type, const QString& name)
{
    titleType_ = type;
    titleName_ = name;
    emit q_ptr->titleContextChanged(q_ptr->panelId(), type, name);
}

QString InspectorPanel::contextType() const
{
    return d->titleType_.toUpper();
}

QString InspectorPanel::contextName() const
{
    return (d->titleType_.isEmpty() && d->titleName_.isEmpty())
        ? tr("Nothing selected") : d->titleName_;
}

QString InspectorPanel::titleObjectName() const
{
    return d->titleName_;
}

void InspectorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    emit pinToggled(panelId_, pinned);
}

ProjectItem* InspectorPanel::currentContextItem() const
{
    using P = InspectorPanelPrivate;
    switch (d->stack_->currentIndex()) {
    case P::PageSound:         return d->currentSoundItem_;
    case P::PageLayer:         return d->currentLayerItem_;
    case P::PageContainer:     return d->currentContainerItem_;
    case P::PageTrack:         return d->currentTrackItem_;
    case P::PageSegment:         return d->currentSegmentItem_;
    case P::PageVariable:      return d->currentVariableItem_;
    case P::PageBus:           return d->currentBusItem_;
    case P::PageAction:        return d->currentActionItem_;
    case P::PageAttenuation:   return d->currentAttenuationItem_;
    default:                   return nullptr;
    }
}

QString InspectorPanel::pinnedObjectGuid() const
{
    ProjectItem* item = currentContextItem();
    return item ? item->guid.toString(QUuid::WithoutBraces) : QString();
}

void InspectorPanel::wireEngineLaunch(PropertyEditorEngine* engine)
{
    connect(engine, &PropertyEditorEngine::launchRequested, this,
            [this](const QString& slot) {
                requestEditorLaunch(slot);
            });
}

void InspectorPanel::requestEditorLaunch(const QString& slot)
{
    emit editorLaunchRequested(
        panelId_, slot, currentContextItem(), placementContext());
}

void InspectorPanel::onSearchTextChanged(const QString& text)
{
    QWidget* current = d->stack_->currentWidget();
    if (!current) return;

    QString lowerText = text.toLower();
    bool isEmpty = text.isEmpty();
    if (!isEmpty)
        emit searchFilterChanged(text);

    std::function<bool(QWidget*)> filter = [&](QWidget* w) -> bool {
        bool visible = false;
        
        bool matches = false;
        
        if (auto gb = qobject_cast<QGroupBox*>(w))
        {
            if (gb->title().toLower().contains(lowerText)) matches = true;
        }
        else if (auto lbl = qobject_cast<QLabel*>(w))
        {
            if (lbl->text().toLower().contains(lowerText)) matches = true;
        }
        else if (auto btn = qobject_cast<QAbstractButton*>(w))
        {
            if (btn->text().toLower().contains(lowerText)) matches = true;
        }
        else if (auto slider = qobject_cast<ParameterSlider*>(w))
        {
        }
        
        if (isEmpty) matches = true;
        
        bool hasVisibleChildren = false;
        const auto children = w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        
        if (!children.isEmpty())
        {
            for (auto child : children)
            {
                if (filter(child)) hasVisibleChildren = true;
            }
            
            if (hasVisibleChildren) visible = true;
        }
        else
        {
            visible = matches;
        }
        
        
        if (matches && qobject_cast<QGroupBox*>(w))
        {
            for (auto child : children) child->setVisible(true);
            visible = true;
        }
        
        w->setVisible(visible);
        return visible;
    };
    
    if (isEmpty)
    {
        for (auto child : current->findChildren<QWidget*>())
            child->setVisible(true);
    }
    else
    {

        auto sections = current->findChildren<CollapsibleSection*>();
        for (auto section : sections)
        {
            // Sections carrying an inline row editor (e.g. the Steps list) own
            // their in-row filtering via searchFilterChanged; never collapse
            // them to empty on a text that only matches a row's contents.
            const bool ownsRowFilter =
                section->findChild<QWidget*>(QStringLiteral("stepsListWidget")) != nullptr;
            if (ownsRowFilter)
            {
                section->setVisible(true);
                section->setExpanded(true);
                continue;
            }
            bool match = section->title().toLower().contains(lowerText);
            for (const auto& field : section->property("inspectorFieldNames").toStringList())
                if (field.toLower().contains(lowerText)) match = true;
            if (!match && section->property("searchDescendants").toBool())
            {
                const auto descendants = section->findChildren<QWidget*>();
                for (QWidget* descendant : descendants)
                {
                    bool hiddenByMode = false;
                    for (QWidget* ancestor = descendant;
                         ancestor && ancestor != section;
                         ancestor = ancestor->parentWidget())
                    {
                        if (ancestor->isHidden())
                        {
                            hiddenByMode = true;
                            break;
                        }
                    }
                    if (hiddenByMode)
                        continue;

                    QString candidate;
                    if (auto* label = qobject_cast<QLabel*>(descendant))
                        candidate = label->text();
                    else if (auto* button = qobject_cast<QAbstractButton*>(descendant))
                        candidate = button->text();
                    const QString qualifiedCandidate =
                        section->title() + QLatin1Char(' ') + candidate;
                    if (qualifiedCandidate.toLower().contains(lowerText))
                    {
                        match = true;
                        break;
                    }
                }
            }
            section->setVisible(match || isEmpty);
            if (match && !isEmpty) section->setExpanded(true);
        }
    }

    if (isEmpty)
        emit searchFilterChanged(text);
}

void InspectorPanel::reapplySearchFilter()
{
    if (d->searchBox_->text().isEmpty())
        return;
    onSearchTextChanged(d->searchBox_->text());
}

void InspectorPanel::onFontSettingsChanged(PanelId panelId, int size)
{
    if (panelId == PanelId::Inspector)
    {
        QFont font = FontManager::instance().getFont(FontRole::Body, size);
        setFont(font);
    }
}

void InspectorPanel::applyTheme()
{
    update();
}

}
