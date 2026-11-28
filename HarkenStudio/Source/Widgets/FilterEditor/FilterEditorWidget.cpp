#include "FilterEditorWidget.h"

#include "FilterResponseCanvas.h"
#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/FilterAuthoring.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"

#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

namespace harken::studio {
namespace {

std::optional<QVector<ProjectPlatformId>> authoringPlatforms()
{
    const auto& context = PlatformContextService::instance().context();
    return context.all || context.selected.isEmpty() ? std::nullopt
        : std::optional<QVector<ProjectPlatformId>>(context.selected);
}

}

FilterEditorWidget::FilterEditorWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("FilterEditorWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("filterEditorContent"));
    QtControlSkin::setSemanticProperty(frame, "surfaceRole", QStringLiteral("panel"));
    auto* content = new QVBoxLayout(frame);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    auto* splitter = new QSplitter(Qt::Vertical, frame);
    splitter->setObjectName(QStringLiteral("filterEditorSplitter"));
    splitter->setProperty("splitterRole", QStringLiteral("border"));
    splitter->setHandleWidth(1);
    splitter->setChildrenCollapsible(false);
    content->addWidget(splitter);
    layout->addWidget(frame);
    canvas_ = new FilterResponseCanvas(splitter);
    splitter->addWidget(canvas_);
    fields_ = new PropertyEditorEngine(this);
    PropertySchema schema;
    for (const auto& descriptor : VMixNode::schema())
        if (FilterSemantics::fieldInfo(descriptor.field)) schema.push_back(descriptor);
    fields_->setItemType(ProjectItem::Type::Sound);
    fields_->setSchema(schema, ComponentType::MixNode);
    fieldsPage_ = fields_->buildPage(this);
    fieldsPage_->layout()->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(splitter);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(fieldsPage_);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({canvas_->sizeHint().height(), canvas_->sizeHint().height()});
    connect(fields_, &PropertyEditorEngine::externalEditorRequested,
            this, &FilterEditorWidget::externalEditorRequested);
    connect(fields_, &PropertyEditorEngine::fieldChanged, this,
        [this](const QString& field, const QVariant&, const QVariant& value) {
            ActionManager::instance().execute(QStringLiteral("set_item_property"),
                fields_->buildSetItemPropertyArgs(field, value, authoringPlatforms()),
                ActionSource::GUI, fields_);
            fields_->refreshField(field);
            refresh(fields_);
        });
    connect(canvas_, &FilterResponseCanvas::editStarted, this, [this] {
        gestureStart_ = canvas_->filter();
        gestureActive_ = true;
    });
    connect(canvas_, &FilterResponseCanvas::sectionEdited, this,
        [this](FilterKind kind, double frequency, double q, double gain) {
            auto* item = ProjectManager::instance().findItemByGuid(ownerGuid_);
            if (!gestureActive_ || !item) return;
            auto filter = canvas_->filter();
            filter.sections[static_cast<size_t>(kind)] = {frequency, q, gain};
            const auto platforms = authoringPlatforms();
            if (!gesture_) {
                gestureBase_ = FilterAuthoring::effectiveState(*item,
                    platforms ? PlatformScope(platforms->front()) : std::nullopt);
                gesture_ = std::make_unique<PropertyEditSession>(item, QStringLiteral("filter"),
                    FilterSemantics::toJson(gestureBase_).toVariantMap(), this, platforms);
            }
            auto next = gestureBase_;
            for (const auto& field : FilterSemantics::fields())
                if (FilterSemantics::value(filter, field) != FilterSemantics::value(gestureStart_, field))
                    FilterSemantics::setValue(next, field, FilterSemantics::value(filter, field));
            gesture_->preview(FilterSemantics::toJson(next).toVariantMap());
        });
    connect(canvas_, &FilterResponseCanvas::editFinished, this, [this] {
        gestureActive_ = false;
        if (gesture_) {
            AutoComplexUndo undo(tr("Edit Filter"));
            if (gesture_->currentValue() == gesture_->startValue()) gesture_->cancel();
            else {
                undo.open();
                gesture_->commit();
            }
            gesture_.reset();
        }
        refresh();
    });
    auto& project = ProjectManager::instance();
    connect(&project, &ProjectManager::activePropertyEditsFinishing, this, [this](bool commitChanges) {
        if (commitChanges) canvas_->commitEdit();
        else cancelGesture();
    });
    const auto propertyChanged = [this](ProjectItem* item, const QString& field, const QObject* originator) {
        if (item && item->guid == ownerGuid_ && FilterAuthoring::isField(field)) refresh(originator);
    };
    connect(&project, &ProjectManager::itemPropertyChanged, this, propertyChanged);
    connect(&project, &ProjectManager::itemPropertyPreviewed, this, propertyChanged);
    connect(&project, &ProjectManager::projectClosed, this, [this] { bindItem(nullptr); });
    connect(&project, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item && item->guid == ownerGuid_) bindItem(nullptr);
    });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged, this, [this] {
        cancelGesture();
        fields_->bindItem(ProjectManager::instance().findItemByGuid(ownerGuid_));
        refresh();
    });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, &FilterEditorWidget::refreshAppearance);
    refreshAppearance();
}

FilterEditorWidget::~FilterEditorWidget()
{
    cancelGesture();
}

void FilterEditorWidget::bindItem(ProjectItem* item)
{
    cancelGesture();
    ownerGuid_ = item ? item->guid : QUuid{};
    fields_->setItemType(item ? item->type : ProjectItem::Type::Sound);
    fields_->bindItem(item);
    refresh();
}

void FilterEditorWidget::cancelGesture()
{
    gestureActive_ = false;
    if (gesture_) gesture_->cancel();
    gesture_.reset();
    canvas_->cancelEdit();
}

void FilterEditorWidget::refresh(const QObject* originator)
{
    auto* item = ProjectManager::instance().findItemByGuid(ownerGuid_);
    if (!item || !item->comp<VMixNode>()) {
        canvas_->setReadOnly(true);
        canvas_->setFilter({});
        return;
    }
    const auto filter = FilterAuthoring::effectiveState(*item,
        currentWorldPlatformScope(PlatformContextService::instance().context()));
    canvas_->setReadOnly(false);
    canvas_->setFilter(filter, ProjectManager::instance().currentProject()->sampleRate);
    if (originator != fields_) fields_->refreshField(QStringLiteral("filter"));
}

void FilterEditorWidget::refreshAppearance()
{
    const auto sections = fieldsPage_->findChildren<CollapsibleSection*>();
    const QStringList keys{QStringLiteral("filter_low_pass"), QStringLiteral("filter_high_pass"),
        QStringLiteral("filter_low_shelf"), QStringLiteral("filter_high_shelf")};
    for (int i = 0; i < keys.size(); ++i)
        for (auto* section : sections)
            if (section->objectName() == QStringLiteral("section_") + keys[i]) {
                const auto color = ThemeManager::instance().colors().categoricalColor(i);
                section->setHeaderTintColor(color);
            }
}

}
