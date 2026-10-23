
#include "PropertyEditorEngine.h"
#include "Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "WidgetFactory.h"
#include "InspectorTabContainer.h"
#include "InspectorLayout.h"
#include "InspectorSlotStrip.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/Shared/PlatformFieldMerge.h"
#include "Drawers/ListFieldDrawer.h"
#include "Core/PlatformContextService.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Core/ProjectTypes.h"
#include "Core/PropertyEditSession.h"
#include "Core/BindingContract.h"
#include "Core/ProjectManager.h"
#include "Core/Component.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VAttenuation.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VSpatial.h"
#include "Core/Components/VTrack.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VVariable.h"
#include "Core/ThemeManager.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QAbstractButton>
#include <QToolButton>
#include <QStackedWidget>
#include <QFrame>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QEvent>
#include <QEnterEvent>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QFont>
#include <QHash>
#include <QJsonArray>
#include <QSignalBlocker>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace VNX {

namespace {

using harkenplayableproperties::PlayablePropertyReferenceRoleV1;
using harkenplayableproperties::PlayablePropertyIdV1;

std::optional<QVector<ProjectPlatformId>> currentAuthoringPlatforms()
{
    const QVector<ProjectPlatformId> selected =
        PlatformContextService::instance().context().selected;
    if (PlatformContextService::instance().context().all || selected.isEmpty())
        return std::nullopt;
    return selected;
}

class MixedAcrossPlatformsCheckBox final : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override
    {
        if (checkState() == Qt::PartiallyChecked) {
            setCheckState(Qt::Checked);
            setTristate(false);
            return;
        }
        QCheckBox::nextCheckState();
    }
};

class ResizablePlainTextEdit final : public QPlainTextEdit
{
public:
    explicit ResizablePlainTextEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
    {
        fieldHeight_ = QtControlSkin::current().fieldHeight();
        setFixedHeight(fieldHeight_);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        setViewportMargins(QtControlSkin::current().fieldTextMargins());
        QtControlSkin::setSemanticProperty(this, "fieldRole",
                                          QStringLiteral("inspectorText"));
        grip_ = new ResizeGrip(this);
        grip_->setObjectName(QStringLiteral("plainTextResizeGrip"));
        positionGrip();
        grip_->hide();
        auto* scrollBar = verticalScrollBar();
        connect(scrollBar, &QScrollBar::rangeChanged, this,
                [this, scrollBar](int minimum, int maximum) {
                    scrollBar->setEnabled(minimum < maximum);
                    positionGrip();
                });
        scrollBar->setEnabled(scrollBar->minimum() < scrollBar->maximum());
        connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this,
                [this] { positionGrip(); });
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this] {
                    setViewportMargins(QtControlSkin::current().fieldTextMargins());
                    const int nextHeight = QtControlSkin::current().fieldHeight();
                    setFixedHeight(height() == fieldHeight_
                        ? nextHeight : std::max(height(), nextHeight));
                    fieldHeight_ = nextHeight;
                });
    }

protected:
    void enterEvent(QEnterEvent* event) override
    {
        grip_->show();
        grip_->raise();
        QPlainTextEdit::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        if (!grip_->isDragging()
            && !rect().contains(mapFromGlobal(QCursor::pos())))
            grip_->hide();
        QPlainTextEdit::leaveEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QPlainTextEdit::resizeEvent(event);
        positionGrip();
    }

private:
    class ResizeGrip final : public QWidget
    {
    public:
        explicit ResizeGrip(ResizablePlainTextEdit* edit)
            : QWidget(edit), edit_(edit)
        {
            setFixedSize(14, 14);
            setCursor(Qt::SizeFDiagCursor);
            setAttribute(Qt::WA_TranslucentBackground);
        }

        bool isDragging() const { return dragging_; }

    protected:
        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() != Qt::LeftButton) return;
            dragging_ = true;
            startGlobalY_ = event->globalPosition().y();
            startHeight_ = edit_->height();
            grabMouse();
            event->accept();
        }

        void mouseMoveEvent(QMouseEvent* event) override
        {
            if (!dragging_) return;
            if (!(event->buttons() & Qt::LeftButton)) {
                finishDragging();
                return;
            }
            const int delta = qRound(event->globalPosition().y() - startGlobalY_);
            edit_->setFixedHeight(std::max(
                edit_->fieldHeight_,
                startHeight_ + delta));
            event->accept();
        }

        void mouseReleaseEvent(QMouseEvent* event) override
        {
            if (!dragging_ || event->button() != Qt::LeftButton) return;
            finishDragging();
            if (!edit_->rect().contains(edit_->mapFromGlobal(QCursor::pos())))
                hide();
            event->accept();
        }

        bool event(QEvent* event) override
        {
            if (event->type() == QEvent::UngrabMouse
                || event->type() == QEvent::Hide
                || event->type() == QEvent::WindowDeactivate)
                cancelDragging();
            return QWidget::event(event);
        }

        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            QColor color = palette().color(QPalette::Text);
            color.setAlphaF(color.alphaF() * 0.45);
            QPen pen(color, 2.0, Qt::SolidLine, Qt::SquareCap,
                     Qt::MiterJoin);
            painter.setPen(pen);
            QPainterPath path;
            path.moveTo(3, 11);
            path.lineTo(11, 11);
            path.lineTo(11, 3);
            painter.drawPath(path);
        }

    private:
        void finishDragging()
        {
            if (!dragging_) return;
            dragging_ = false;
            if (QWidget::mouseGrabber() == this)
                releaseMouse();
        }

        void cancelDragging()
        {
            if (!dragging_) return;
            dragging_ = false;
        }

        ResizablePlainTextEdit* edit_ = nullptr;
        bool dragging_ = false;
        qreal startGlobalY_ = 0.0;
        int startHeight_ = 0;
    };

    void positionGrip()
    {
        const QRect contentRect = viewport()->geometry().marginsAdded(viewportMargins());
        grip_->move(contentRect.right() - grip_->width() + 1,
                    contentRect.bottom() - grip_->height() + 1);
    }

    ResizeGrip* grip_ = nullptr;
    int fieldHeight_ = 0;
};

bool objectReferenceRole(PlayablePropertyReferenceRoleV1 role)
{
    return role == PlayablePropertyReferenceRoleV1::MainOutputBus
        || role == PlayablePropertyReferenceRoleV1::Attenuation;
}

QString drawerMixedTooltip(ProjectItem& owner,
                           const PlayablePropertyLocatorV1& locator,
                           uint8_t propertyId)
{
    const PlatformContext& context = PlatformContextService::instance().context();
    if (context.selected.isEmpty())
        return {};
    const PlatformFieldView view = platformFieldView(
        owner, locator, propertyId, context);
    if (view.uniform)
        return {};
    return platformFieldMixedTooltip(view, ProjectManager::instance().currentProject()->platforms);
}

void applyDrawerMixedTooltip(QWidget* widget,
                             QString& lastMixed,
                             ProjectItem* owner,
                             const PlayablePropertyLocatorV1& locator,
                             uint8_t propertyId)
{
    if (!widget) return;
    const QString mixed = owner
        ? drawerMixedTooltip(*owner, locator, propertyId)
        : QString();
    if (!mixed.isEmpty()) {
        widget->setToolTip(mixed);
        lastMixed = mixed;
        return;
    }
    if (!lastMixed.isEmpty() && widget->toolTip() == lastMixed)
        widget->setToolTip(QString());
    lastMixed.clear();
}

bool itemMatchesReferenceRole(const ProjectItem& item,
                              PlayablePropertyReferenceRoleV1 role)
{
    return (role == PlayablePropertyReferenceRoleV1::MainOutputBus
            && item.type == ProjectItem::Type::Bus)
        || (role == PlayablePropertyReferenceRoleV1::Attenuation
            && item.type == ProjectItem::Type::Attenuation);
}

std::optional<QJsonValue> editorWireValue(
    const PlayablePropertyAuthoringV1::Field& field, const QVariant& value)
{
    if (!objectReferenceRole(field.referenceRole))
        return PlayablePropertyAuthoringV1::wireValueFromVariant(field.id, value);
    bool ok = false;
    const qulonglong authoredId = value.toULongLong(&ok);
    if (!ok || authoredId > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    if (authoredId == 0) return QJsonValue(objectIdString({}));
    ProjectItem* item = ProjectManager::instance().findItemByObjectID(
        static_cast<uint32_t>(authoredId));
    if (!item || harkenstudio::IsNull(item->runtimeId())
        || !itemMatchesReferenceRole(*item, field.referenceRole)
        || ProjectManager::instance().findItemByRuntimeStableId(
               item->runtimeId()) != item)
        return std::nullopt;
    return QJsonValue(objectIdString(item->runtimeId()));
}

std::optional<QVariant> editorValue(
    const PlayablePropertyAuthoringV1::Field& field, const QJsonValue& value)
{
    if (!objectReferenceRole(field.referenceRole)) return value.toVariant();
    if (!value.isString()) return std::nullopt;
    harkenstudio::ObjectId stableId{};
    if (!parseObjectIdJson(value, &stableId)) return std::nullopt;
    if (harkenstudio::IsNull(stableId)) return QVariant::fromValue(static_cast<uint32_t>(0));
    ProjectItem* item = ProjectManager::instance().findItemByRuntimeStableId(stableId);
    if (!item || !itemMatchesReferenceRole(*item, field.referenceRole))
        return QVariant(value.toString());
    return QVariant::fromValue(item->objectID);
}

QVariant playableBaseEditorValue(ProjectItem& owner,
                                 const PlayablePropertyLocatorV1& locator,
                                 const PlayablePropertyAuthoringV1::Field& property)
{
    if (PlayablePropertyAuthoringV1::isComponentStored(property.id)) {
        if (const auto* project = ProjectManager::instance().currentProject()) {
            const auto index = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*project);
            if (index && index->valid)
                return HarkenEditor::Bridge::resolvedStudioSpatializationMix(owner, *index);
        }
        const auto source = PlayablePropertyAuthoringV1::resolveSpatializationMix(owner);
        return source ? source->state.value.toVariant() : QVariant(1.0f);
    }
    const auto state = PlayablePropertyAuthoringV1::readProperty(
        owner, locator, property.id, std::nullopt);
    if (!state) return {};
    const auto value = editorValue(property, state->value);
    return value ? *value : QVariant();
}

}

PropertyEditorEngine::PropertyEditorEngine(QObject* parent)
    : QObject(parent)
{
    connect(&ProjectManager::instance(), &ProjectManager::fieldGroupsPreviewed,
            this, &PropertyEditorEngine::refreshFieldGroupPreview);
    connect(&ProjectManager::instance(), &ProjectManager::activePropertyEditsFinishing,
            this, [this](bool commitChanges) {
        if (!commitChanges) {
            cancelGroupEdits();
            return;
        }
        QList<QPointer<ParameterBinding>> pending;
        for (const auto& binding : bindings_) {
            if (!fieldGroup(binding.field) || !binding.widget) continue;
            if (binding.parameterBinding) pending.append(binding.parameterBinding);
            else
                for (auto* parameter : binding.widget->findChildren<ParameterBinding*>())
                    pending.append(parameter);
        }
        for (const auto& parameter : pending)
            if (parameter && parameter->isEditing()) parameter->commitEdit();
    });
    connect(&PlatformContextService::instance(),
            &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) {
        if (!currentItem_) return;
        cancelGroupEdits();
        for (auto& binding : bindings_) {
            if (binding.parameterBinding && binding.parameterBinding->isEditing())
                binding.parameterBinding->cancelEdit();
            else if (auto* segmented = qobject_cast<ValueSegmentedControl*>(binding.widget.data()))
                segmented->cancelNumericEdit();
            binding.editSession.reset();
        }
        for (const auto& binding : bindings_) {
            if (fieldGroup(binding.field)) continue;
            if (!PlayablePropertyAuthoringV1::fieldByName(binding.field)
                && binding.field != QLatin1String("inputs")
                && binding.field != QLatin1String("receives"))
                continue;
            refreshField(binding.field);
        }
        refreshFieldGroups();
        for (const auto& section : sectionBindings_) {
            if (section.provider && section.widget)
                section.provider->bind(section.widget, currentItem_);
        }
    });
}

PropertyEditorEngine::~PropertyEditorEngine()
{
    releasePage();
}

void PropertyEditorEngine::releasePage()
{
    cancelGroupEdits();
    for (auto& binding : bindings_) {
        if (binding.parameterBinding && binding.parameterBinding->isEditing())
            binding.parameterBinding->cancelEdit();
        binding.editSession.reset();
    }
    currentItem_ = nullptr;
    bindings_.clear();
    sectionBindings_.clear();
    groupBindings_.clear();
    groupPreviews_.clear();
}

std::optional<PlayablePropertyLocatorV1>
PropertyEditorEngine::editablePlayableLocator(
    const QString& field, ProjectItem** owner) const
{
    if (owner) *owner = nullptr;
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    if (!currentItem_ || !property)
        return std::nullopt;
    if (currentItem_->type == ProjectItem::Type::Bus
        && property->id == static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume)) {
        if (owner) *owner = currentItem_;
        return std::nullopt;
    }
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*currentItem_);
    if (!locator) return std::nullopt;
    if (owner) *owner = currentItem_;
    return locator;
}

QJsonObject PropertyEditorEngine::buildSetItemPropertyArgs(
    const QString& field,
    const QVariant& value,
    std::optional<QVector<ProjectPlatformId>> platforms) const
{
    ProjectItem* owner = currentItem_;
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    const auto locator = property
        ? editablePlayableLocator(field, &owner)
        : std::nullopt;
    QJsonObject args{
        {QStringLiteral("authoring_handle"),
         owner ? static_cast<qint64>(owner->objectID) : 0},
        {QStringLiteral("property"), field},
        {QStringLiteral("value"), QJsonValue::fromVariant(value)}};
    if (platforms && !(property && PlayablePropertyAuthoringV1::isComponentStored(property->id))) {
        QJsonArray names;
        for (ProjectPlatformId platform : *platforms)
            names.append(ProjectPlatformSemantics::idString(platform));
        args[QStringLiteral("platform_ids")] = names;
    }
    if (!locator) return args;
    const auto wireValue = property
        ? editorWireValue(*property, value)
        : std::nullopt;
    if (!wireValue) {
        args.remove(QStringLiteral("value"));
        return args;
    }
    args[QStringLiteral("value")] = *wireValue;
    args[QStringLiteral("override_operation")] = QStringLiteral("set_override");
    return args;
}

void PropertyEditorEngine::setSchema(const PropertySchema& schema, ComponentType componentType)
{
    schema_ = schema;
    componentType_ = componentType;
}

QString PropertyEditorEngine::widgetTypeFor(const PropertyDescriptor& d)
{
    if (d.flags.segmented)
        return d.segments.empty() ? QStringLiteral("segmented")
                                  : QStringLiteral("value_segmented");

    const PropType t = d.type;
    switch (t)
    {
        case PropType::Float: return QStringLiteral("slider");
        case PropType::Int:   return QStringLiteral("int_spinbox");
        case PropType::Bool:  return QStringLiteral("checkbox");
        case PropType::Enum:  return QStringLiteral("combo");
        case PropType::String: return QStringLiteral("text");
        case PropType::Color:
        case PropType::ObjectRef:
        default: return {};
    }
}

QJsonObject PropertyEditorEngine::synthConfig(const PropertyDescriptor& d)
{
    QJsonObject cfg;
    cfg[QStringLiteral("label")] = QString();
    if (d.min.isValid()) cfg[QStringLiteral("min")] = d.min.toDouble();
    if (d.max.isValid()) cfg[QStringLiteral("max")] = d.max.toDouble();
    cfg[QStringLiteral("decimals")] = d.decimals;
    if (d.displayScale != 1.0)
        cfg[QStringLiteral("displayScale")] = d.displayScale;
    if (!d.unit.isEmpty()) cfg[QStringLiteral("suffix")] = d.unit;
    if (!d.specialValueText.isEmpty())
        cfg[QStringLiteral("specialValueText")] = d.specialValueText;
    cfg[QStringLiteral("objectName")] = QStringLiteral("prop_") + d.field;

    if (d.type == PropType::Enum)
    {
        QJsonArray opts;
        for (const auto& o : d.options)
        {
            QJsonObject oo;
            oo[QStringLiteral("text")]  = o.text;
            oo[QStringLiteral("value")] = QJsonValue::fromVariant(o.value);
            opts.append(oo);
        }
        cfg[QStringLiteral("options")] = opts;
    }

    if (!d.segments.empty())
    {
        QJsonArray segs;
        for (const auto& s : d.segments)
        {
            QJsonObject so;
            so[QStringLiteral("text")]    = s.text;
            so[QStringLiteral("value")]   = QJsonValue::fromVariant(s.value);
            so[QStringLiteral("numeric")] = s.numeric;
            if (s.numericMin.isValid())
                so[QStringLiteral("numericMin")] = QJsonValue::fromVariant(s.numericMin);
            if (s.numericMax.isValid())
                so[QStringLiteral("numericMax")] = QJsonValue::fromVariant(s.numericMax);
            segs.append(so);
        }
        cfg[QStringLiteral("segments")] = segs;
    }
    return cfg;
}

QWidget* PropertyEditorEngine::buildField(const PropertyDescriptor& d, QWidget* parent)
{
    auto& reg = InspectorCustomizationRegistry::instance();

    QWidget* control = nullptr;
    QString  type;
    IFieldDrawer* drawer = reg.fieldDrawer(d.type);
    if (drawer)
    {
        auto descriptor = d;
        if (fieldGroup(d.field)) {
            const auto selectable = [self = QPointer<PropertyEditorEngine>(this), field = d.field](uint32_t, uint32_t candidate) {
                return self && self->groupReferenceSelectable(field, candidate);
            };
            if (d.type == PropType::ObjectRef) descriptor.refSelectable = selectable;
            for (auto& element : descriptor.elementSchema)
                if (element.type == PropType::ObjectRef) element.refSelectable = selectable;
        }
        control = drawer->build(descriptor, parent);
    }
    else
    {
        if (d.type == PropType::String && d.flags.multiline)
            type = QStringLiteral("multiline_text");
        else
            type = widgetTypeFor(d);
        if (d.type == PropType::String && !d.flags.multiline)
            return nullptr;
        if (type.isEmpty()) return nullptr;
        if (type == QLatin1String("checkbox")) {
            auto* check = new MixedAcrossPlatformsCheckBox(parent);
            check->setObjectName(QStringLiteral("prop_") + d.field);
            control = check;
        } else if (type == QLatin1String("multiline_text")) {
            auto* edit = new ResizablePlainTextEdit(parent);
            edit->setObjectName(QStringLiteral("prop_") + d.field);
            edit->installEventFilter(this);
            control = edit;
        } else {
            control = WidgetFactory::instance().create(type, synthConfig(d), parent);
        }
    }
    if (!control) return nullptr;
    if (const auto* field = PlayablePropertyAuthoringV1::fieldByName(d.field);
        field && objectReferenceRole(field->referenceRole))
        control->setProperty("generatedPlayableReference", true);
    if (auto* slider = qobject_cast<ParameterSlider*>(control))
    {
        slider->binding()->setDefaultValue(d.def.isValid() ? d.def.toDouble() : 0.0);
        slider->binding()->setName(d.label.isEmpty() ? d.field : d.label);
    }
    if (d.flags.readOnly && d.type != PropType::List && d.type != PropType::ObjectRef)
        control->setEnabled(false);

    const auto& inspector = ThemeManager::instance().metrics().inspector;
    auto* strip = new InspectorSlotStrip(parent);
    if (BindingContractSemantics::isBindable(
            BindingContractSemantics::derive(d, itemType_))) {
        QWidget* widget = perPageBinding_
            ? perPageBinding_(d)
            : reg.makeSlotDecorator(InspectorSlotKind::Binding, d);
        if (widget) strip->addSlot(InspectorSlotKind::Binding, widget);
    }
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(itemType_)) {
        if (auto* widget = reg.makeSlotDecorator(InspectorSlotKind::Attenuation, d))
            strip->addSlot(InspectorSlotKind::Attenuation, widget);
    }
    QWidget* decoratorWidget = strip->occupiedSlotCount() > 0 ? strip : nullptr;
    if (!decoratorWidget) {
        delete strip;
        strip = nullptr;
    }
    IFieldDecorator* decorator = strip;

    const bool isBool       = (type == QStringLiteral("checkbox"));
    const bool valueBearing = (type == QStringLiteral("int_spinbox"));
    QWidget* row = new QWidget(parent);
    if (d.type != PropType::List)
        row->setMinimumHeight(inspector.rowHeight);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    DebugTint::tintContainer(row, DebugTint::Role::Row);

    const bool hideLabel = d.type == PropType::List;
    control->setProperty("inspectorField", d.field);
    if (!hideLabel)
    {
        auto* labelW = new QLabel(d.label.isEmpty() ? d.field : d.label, row);
        labelW->setObjectName(QStringLiteral("label_") + d.field);
        labelW->setAlignment(d.flags.multiline
            ? (Qt::AlignLeft | Qt::AlignTop)
            : (Qt::AlignLeft | Qt::AlignVCenter));
        QtControlSkin::setSemanticProperty(labelW, "textRole",
                                          QStringLiteral("formLabel"));
        DebugTint::tintBare(labelW, DebugTint::Role::Label);
        labelW->setToolTip(labelW->text());
        labelW->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        labelW->setContentsMargins(0, 0, inspector.columnPadding, 0);
        h->addWidget(labelW, inspector.labelColumnShare);
    }

    // Every editor occupies one shared value column: label and value split the
    // row proportionally (labelColumnShare against valueColumnShare), so narrowing
    // the panel shrinks both columns in ratio instead of collapsing the value
    // alone; both cells are Ignored so the split never tracks content and every
    // field type lines up across rows. The column carries
    // its own horizontal padding (kColPadX per side) inside a transparent cell,
    // so the value insets symmetrically with the label's right pad and the
    // gutter's left pad while the row keeps zero spacing. Declaring the editor
    // Expanding — not merely handing the layout a stretch factor — is what makes
    // it fill inside the inspector's nested scroll-area/tab chain; passing an
    // alignment instead pins the editor at its natural size (used by checkbox).
    auto fillValueColumn = [&](QWidget* w, Qt::Alignment align = {}) {
        auto* cell = new QWidget(row);
        DebugTint::tintContainer(cell, DebugTint::Role::Flex);
        cell->setSizePolicy(QSizePolicy::Ignored, cell->sizePolicy().verticalPolicy());
        auto* cl = new QHBoxLayout(cell);
        const int leftPad = d.type == PropType::List ? 0 : inspector.columnPadding;
        cl->setContentsMargins(leftPad, 0, inspector.columnPadding, 0);
        cl->setSpacing(inspector.columnPadding);
        if (align)
        {
            cl->addWidget(w, 0, align);
        }
        else
        {
            w->setSizePolicy(QSizePolicy::Expanding, w->sizePolicy().verticalPolicy());
            cl->addWidget(w, 1);
        }
        h->addWidget(cell, inspector.valueColumnShare);
    };

    if (isBool)
    {
        QtControlSkin::setSemanticProperty(control, "fieldRole",
                                          QStringLiteral("inspector"));
        fillValueColumn(control, Qt::AlignLeft);
    }
    else if (valueBearing)
    {
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(innerWidget(control, type)))
        {
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            spin->setFont(FontManager::instance().getFont(FontRole::Mono));
            QtControlSkin::setSemanticProperty(spin, "fieldRole",
                                              QStringLiteral("inspectorValue"));
        }
        fillValueColumn(control);
    }
    else if (type == QStringLiteral("slider"))
    {
        if (auto* ps = qobject_cast<ParameterSlider*>(control))
        {
            ps->setCompactValueColumn(inspector.valueColumnWidth);
            ps->setValueColumnStretch(7, 3);
        }
        fillValueColumn(control);
    }
    else
    {
        fillValueColumn(control);
    }

    {
        auto* gutter = new QWidget(row);
        gutter->setObjectName(QStringLiteral("inspectorFieldGutter_") + d.field);
        gutter->setProperty("columnRole", QStringLiteral("gutter"));
        DebugTint::tintContainer(gutter, DebugTint::Role::Gutter);
        const int fullWidth = inspector.slotWidthFor(inspectorFormSlotCount(parent));
        gutter->setFixedWidth(fullWidth);
        gutter->setFixedHeight(inspector.rowHeight);
        auto* gl = new QHBoxLayout(gutter);
        const int leftPadding = inspector.slotSpacing / 2;
        gl->setContentsMargins(leftPadding, 0, inspector.slotSpacing - leftPadding, 0);
        gl->setSpacing(0);
        if (decoratorWidget)
            gl->addWidget(decoratorWidget, 1, Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(gutter);
    }

    FieldBinding b;
    b.field = d.field;
    b.fieldLabel = d.label.isEmpty() ? d.field : d.label;
    b.ownerComponent = d.ownerComponent;
    b.widgetType = type;
    b.type = d.type;
    b.widget = control;
    if (auto* slider = qobject_cast<ParameterSlider*>(control))
        b.parameterBinding = slider->binding();
    b.drawer = drawer;
    b.decorator = decorator;
    if (strip) {
        for (InspectorSlotKind kind : {
                 InspectorSlotKind::Binding,
                 InspectorSlotKind::Attenuation}) {
            if (auto* widget = strip->slotWidget(kind))
                if (auto* slotDecorator = dynamic_cast<IFieldDecorator*>(widget))
                    b.decorators.push_back(slotDecorator);
        }
    }
    b.bindingAddress = BindingContractSemantics::addressFromDescriptor(d, itemType_);
    b.editCondition = d.editCondition;
    b.domain = d.domain;
    b.readOnly = d.flags.readOnly;
    bindings_.push_back(std::move(b));

    if (drawer)
    {
        const QString field = d.field;
        drawer->connectChanged(control, [this, field](const QVariant& v) {
            commitFieldChange(field, v);
        });
        if (field == QLatin1String("sends") && itemType_ == ProjectItem::Type::Bus) {
            const auto self = QPointer<PropertyEditorEngine>(this);
            ListFieldDrawer::setWidgetPreviewCallbacks(control,
                [self, field](const QVariant& value) {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.field == field) { self->previewBusSends(entry, value); return; }
                },
                [self, field] {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.field == field) {
                            if (entry.editSession) entry.editSession->cancel();
                            entry.editSession.reset();
                            return;
                        }
                });
        }
    }
    else
    {
        connectWidgetSignal(control, type, d.field);
    }

    QWidget* fieldRoot = row;
    std::vector<QWidget*> details;
    if (strip)
        details = strip->detailWidgets();
    else if (decorator)
        if (QWidget* detail = decorator->detailWidget())
            details.push_back(detail);
    if (!details.empty())
    {
        auto* wrapper = new QWidget(parent);
        auto* wrapperLayout = new QVBoxLayout(wrapper);
        wrapperLayout->setContentsMargins(0, 0, 0, 0);
        wrapperLayout->setSpacing(0);
        row->setParent(wrapper);
        wrapperLayout->addWidget(row);
        for (QWidget* detail : details) {
            detail->setParent(wrapper);
            wrapperLayout->addWidget(detail);
        }
        fieldRoot = wrapper;
    }

    bindings_.back().topicHost = fieldRoot;
    tagFieldTopic(bindings_.back(), currentItem_);
    return fieldRoot;
}

void PropertyEditorEngine::tagFieldTopic(const FieldBinding& binding, ProjectItem* owner)
{
    if (!binding.topicHost) return;

    ContextInfo::tag(binding.topicHost,
                     ContextTopic::forField(binding.ownerComponent, binding.field, binding.fieldLabel,
                                            owner ? owner->guid : QUuid(),
                                            owner ? owner->type : itemType_,
                                            owner ? owner->name : QString()));
}

void PropertyEditorEngine::tagCustomFields(ProjectItem* owner)
{
    for (const auto& section : sectionBindings_) {
        if (!section.widget) continue;
        auto widgets = section.widget->findChildren<QWidget*>();
        widgets.prepend(section.widget);
        if (section.header) {
            widgets.append(section.header);
            widgets.append(section.header->findChildren<QWidget*>());
        }
        for (const auto& field : schema_) {
            if (field.section != section.section) continue;
            const auto found = std::find_if(widgets.begin(), widgets.end(), [&](QWidget* widget) {
                return widget->property("inspectorField").toString() == field.field;
            });
            if (found == widgets.end())
                throw std::invalid_argument("Custom Inspector field has no editor");
            ContextInfo::tag(*found, ContextTopic::forField(
                field.ownerComponent, field.field, field.label,
                owner ? owner->guid : QUuid(), owner ? owner->type : itemType_,
                owner ? owner->name : QString()));
        }
    }
}

QWidget* PropertyEditorEngine::buildPage(QWidget* parent)
{
    validateInspectorSchema(schema_);
    bindings_.clear();
    sectionBindings_.clear();
    groupBindings_.clear();

    const auto& inspector = ThemeManager::instance().metrics().inspector;
    auto* page = new QWidget(parent);
    setInspectorFormSlotCount(page, 2);
    page->setFont(FontManager::instance().getFont(FontRole::Caption));
    const bool tabbed = custom_ && custom_->tabbedLayout;
    auto* layout = new QVBoxLayout(page);
    const int pageMargin = tabbed ? 0 : inspector.pageMargin;
    layout->setContentsMargins(pageMargin, pageMargin, pageMargin, pageMargin);
    layout->setSpacing(inspector.sectionSpacing);

    struct TabBucket { QVBoxLayout* lay = nullptr; int n = 0; };
    std::array<TabBucket, kInspectorCategoryCount> buckets{};
    auto bucketLayout = [&](PropertyCategory category) -> QVBoxLayout* {
        auto& b = buckets[static_cast<size_t>(category)];
        if (!b.lay)
        {
            b.lay = new QVBoxLayout();
            b.lay->setContentsMargins(0, 0, 0, 0);
            b.lay->setSpacing(inspector.sectionSpacing);
        }
        return b.lay;
    };
    auto addSection = [&](PropertyCategory category, QWidget* w) {
        if (tabbed) { bucketLayout(category)->addWidget(w); buckets[static_cast<size_t>(category)].n++; }
        else        { layout->addWidget(w); }
    };

    auto& reg = InspectorCustomizationRegistry::instance();
    for (const auto& info : inspectorSections()) {
        const bool present = std::any_of(schema_.begin(), schema_.end(), [&](const auto& field) {
            return field.section == info.section;
        });
        if (!present) continue;
        auto* section = new CollapsibleSection(info.title, 100, page);
        section->setObjectName(QStringLiteral("section_") + info.key);
        QStringList fieldNames;
        for (const auto& field : schema_)
            if (field.section == info.section) fieldNames << field.field << field.label;
        section->setProperty("inspectorFieldNames", fieldNames);
        auto* content = new QVBoxLayout();
        content->setSpacing(inspector.rowSpacing);
        if (auto* provider = reg.sectionProvider(itemType_, info.key)) {
            auto* widget = provider->build(section);
            content->addWidget(widget);
            auto* header = provider->buildHeader(widget, section);
            section->setHeaderWidget(header);
            sectionBindings_.push_back({provider, widget, info.section, header});
            section->setDynamicContent(true);
            section->setProperty("searchDescendants", true);
        } else {
            for (const auto& field : schema_) {
                if (field.section != info.section) continue;
                if (auto* widget = buildField(field, section))
                    content->addWidget(widget);
            }
        }
        section->setContentLayout(content);
        section->setExpanded(true);
        std::set<FieldGroupId> inheritedGroups;
        for (const auto& field : schema_) {
            if (field.section != info.section) continue;
            const auto group = fieldGroup(field.field);
            if (group) inheritedGroups.insert(*group);
        }
        for (const auto group : inheritedGroups)
            buildGroupHeader(group, section, inheritedGroups.size() > 1);
        addSection(info.category, section);
    }
    tagCustomFields(nullptr);

    if (tabbed)
    {
        auto* tabs = new InspectorTabContainer(page);
        for (int i = 0; i < kInspectorCategoryCount; ++i)
        {
            auto& b = buckets[static_cast<size_t>(i)];
            if (!b.lay || b.n == 0) { delete b.lay; continue; }
            const auto category = static_cast<PropertyCategory>(i);
            b.lay->addStretch(1);
            auto* tabPage = new QWidget(tabs);
            tabPage->setLayout(b.lay);
            tabs->addTab(tabPage, inspectorCategoryTitle(category), inspectorCategoryObjectName(category));
        }
        layout->addWidget(tabs, 1);
    }
    else
    {
        layout->addStretch(1);
    }
    return page;
}

void PropertyEditorEngine::connectWidgetSignal(QWidget* widget, const QString& widgetType, const QString& field)
{
    if (widgetType == QStringLiteral("slider"))
    {
        auto* slider = qobject_cast<ParameterSlider*>(widget);
        if (!slider) return;

        ParameterBinding* binding = slider->binding();
        connect(binding, &ParameterBinding::editBegan, this, [this, binding, field](double startValue) {
            if (updating_ || !currentItem_) return;
            for (auto& entry : bindings_)
            {
                if (entry.parameterBinding != binding) continue;
                if (!canEditField(entry)) {
                    binding->cancelEdit();
                    return;
                }
                if (const auto group = fieldGroup(field)) {
                    entry.groupEditArgs = groupBindings_.at(*group).view.actionArgs();
                    entry.groupEditRevision = groupEditRevision_;
                    return;
                }
                ProjectItem* owner = nullptr;
                const auto locator = editablePlayableLocator(field, &owner);
                const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
                if (locator && property) {
                    entry.editSession = std::make_unique<PropertyEditSession>(
                        owner, *locator, property->id, this,
                        currentAuthoringPlatforms());
                } else {
                    entry.editSession = std::make_unique<PropertyEditSession>(
                        currentItem_, field, QVariant(startValue), this);
                }
                return;
            }
        });
        connect(binding, &ParameterBinding::editPreviewed, this, [this, binding](double value) {
            for (auto& entry : bindings_)
            {
                if (entry.parameterBinding == binding && fieldGroup(entry.field)) {
                    previewGroupField(entry, value);
                    return;
                }
                if (entry.parameterBinding == binding && entry.editSession)
                {
                    entry.editSession->preview(QVariant(value));
                    return;
                }
            }
        });
        connect(binding, &ParameterBinding::editEnded, this,
                [this, binding, field](double, double value, bool committed) {
            for (auto& entry : bindings_)
            {
                if (entry.parameterBinding == binding && fieldGroup(field)) {
                    auto args = std::move(entry.groupEditArgs);
                    entry.groupEditArgs.reset();
                    if (committed && args && entry.groupEditRevision == groupEditRevision_)
                        commitFieldChange(field, QVariant(value), args);
                    finishGroupPreview(entry);
                    if (!updating_) refreshFieldGroups();
                    return;
                }
                if (entry.parameterBinding != binding || !entry.editSession) continue;
                if (committed) entry.editSession->commit();
                else           entry.editSession->cancel();
                entry.editSession.reset();
                refreshField(field);
                return;
            }
        });
        connect(binding, &ParameterBinding::valueCommitted, this, [this, field](double value) {
            commitFieldChange(field, QVariant(value));
        });
        return;
    }

    if (widgetType == QStringLiteral("int_spinbox"))
    {
        if (auto* spin = qobject_cast<QSpinBox*>(innerWidget(widget, widgetType)))
            connect(spin, &QSpinBox::valueChanged, this, [this, field](int val) {
                commitFieldChange(field, QVariant(val));
            });
        return;
    }

    if (widgetType == QStringLiteral("checkbox"))
    {
        if (auto* check = qobject_cast<QCheckBox*>(widget))
            connect(check, &QCheckBox::clicked, this, [this, field, check](bool) {
                if (updating_ || !currentItem_) return;
                if (check->checkState() == Qt::PartiallyChecked)
                    return;
                commitFieldChange(field, QVariant(check->checkState() == Qt::Checked));
            });
        return;
    }

    if (widgetType == QStringLiteral("combo"))
    {
        auto* combo = qobject_cast<QComboBox*>(innerWidget(widget, widgetType));
        if (combo)
            connect(combo, &QComboBox::currentIndexChanged, this, [this, field, combo](int idx) {
                if (updating_ || !currentItem_ || idx < 0) return;
                commitFieldChange(field, combo->itemData(idx));
            });
        return;
    }

    if (widgetType == QStringLiteral("segmented"))
    {
        auto* control = qobject_cast<SegmentedControl*>(widget);
        auto* group = control ? control->buttonGroup() : nullptr;
        if (!group)
            return;
        connect(group, qOverload<QAbstractButton*>(&QButtonGroup::buttonClicked),
                this, [this, field](QAbstractButton* button) {
            if (updating_ || !currentItem_ || !button)
                return;
            commitFieldChange(field, button->property("segmentValue"));
        });
        return;
    }

    // A segment click is one whole decision, so it commits on the spot like the
    // fixed segmented row. The numeric segment's inline editor instead runs the
    // slider's three-phase session: every keystroke previews so the count is
    // audible while typing, and the whole edit still collapses into one undo.
    if (widgetType == QStringLiteral("value_segmented"))
    {
        auto* control = qobject_cast<ValueSegmentedControl*>(widget);
        if (!control)
            return;

        connect(control, &ValueSegmentedControl::segmentPicked,
                this, [this, field](const QVariant& value) {
            if (updating_ || !currentItem_) return;
            commitFieldChange(field, value);
        });
        connect(control, &ValueSegmentedControl::numericEditBegan,
                this, [this, control, field] {
            if (updating_ || !currentItem_) return;
            for (auto& entry : bindings_)
            {
                if (entry.widget != control) continue;
                ProjectItem* owner = nullptr;
                const auto locator = editablePlayableLocator(field, &owner);
                const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
                if (locator && property) {
                    entry.editSession = std::make_unique<PropertyEditSession>(
                        owner, *locator, property->id, this,
                        currentAuthoringPlatforms());
                } else {
                    entry.editSession = std::make_unique<PropertyEditSession>(
                        currentItem_, field, readField(field), this);
                }
                return;
            }
        });
        connect(control, &ValueSegmentedControl::numericPreviewed,
                this, [this, control](const QVariant& value) {
            for (auto& entry : bindings_)
            {
                if (entry.widget == control && entry.editSession)
                {
                    entry.editSession->preview(value);
                    return;
                }
            }
        });
        connect(control, &ValueSegmentedControl::numericEditEnded,
                this, [this, control, field](bool committed) {
            for (auto& entry : bindings_)
            {
                if (entry.widget != control || !entry.editSession) continue;
                if (committed) entry.editSession->commit();
                else           entry.editSession->cancel();
                entry.editSession.reset();
                refreshField(field);
                return;
            }
        });
        return;
    }

    if (widgetType == QStringLiteral("text"))
    {
        if (auto* edit = qobject_cast<QLineEdit*>(widget))
            connect(edit, &QLineEdit::editingFinished, this, [this, field, edit] {
                if (updating_ || !currentItem_) return;
                commitFieldChange(field, edit->text());
            });
        return;
    }
}

bool PropertyEditorEngine::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut && !updating_ && currentItem_)
    {
        for (const auto& binding : bindings_)
        {
            if (binding.widget != watched
                || binding.widgetType != QLatin1String("multiline_text"))
                continue;
            auto* edit = qobject_cast<QPlainTextEdit*>(watched);
            const QVariant oldValue = readField(binding.field);
            if (edit && edit->toPlainText() != oldValue.toString())
                commitFieldChange(binding.field, edit->toPlainText());
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

bool PropertyEditorEngine::applyPlayableFieldView(const FieldBinding& binding)
{
    if (!binding.widget || binding.drawer) return false;
    ProjectItem* owner = nullptr;
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(binding.field);
    const auto locator = property
        ? editablePlayableLocator(binding.field, &owner)
        : std::nullopt;
    if (!property || !locator || !owner) return false;

    if (PlayablePropertyAuthoringV1::isComponentStored(property->id)) {
        setWidgetValue(binding.widget, binding.widgetType,
                       playableBaseEditorValue(*owner, *locator, *property));
        return true;
    }

    const PlatformContext context = PlatformContextService::instance().context();
    if (context.selected.isEmpty()) {
        const auto state = PlayablePropertyAuthoringV1::readProperty(
            *owner, *locator, property->id, std::nullopt);
        if (!state) return false;
        const auto value = editorValue(*property, state->value);
        if (!value) return false;
        setWidgetValue(binding.widget, binding.widgetType, *value);
        return true;
    }

    PlatformFieldView view = platformFieldView(
        *owner, *locator, property->id, context);
    if (view.uniform) {
        const auto value = editorValue(*property, view.value.value);
        if (!value) return false;
        setWidgetValue(binding.widget, binding.widgetType, *value);
        if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data()))
            slider->setToolTip(QString());
        else if (auto* check = qobject_cast<QCheckBox*>(binding.widget.data()))
            check->setToolTip(QString());
        else if (auto* combo = qobject_cast<QComboBox*>(
                     innerWidget(binding.widget, binding.widgetType)))
            combo->setToolTip(QString());
        else if (auto* segmented = qobject_cast<ValueSegmentedControl*>(binding.widget.data()))
            segmented->setToolTip(QString());
        return true;
    }

    const PropertyDescriptor* descriptor = nullptr;
    for (const auto& row : schema_) {
        if (row.field == binding.field) {
            descriptor = &row;
            break;
        }
    }
    const QString tooltip = platformFieldMixedTooltip(view, ProjectManager::instance().currentProject()->platforms, descriptor);
    if (binding.widgetType == QLatin1String("slider")) {
        if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data())) {
            slider->setMixedValue(true);
            slider->setToolTip(tooltip);
        }
        return true;
    }
    if (binding.widgetType == QLatin1String("checkbox")) {
        if (auto* check = qobject_cast<QCheckBox*>(binding.widget.data())) {
            QSignalBlocker blocker(check);
            check->setTristate(true);
            check->setCheckState(Qt::PartiallyChecked);
            check->setToolTip(tooltip);
        }
        return true;
    }
    if (binding.widgetType == QLatin1String("combo")) {
        if (auto* combo = qobject_cast<QComboBox*>(
                innerWidget(binding.widget, binding.widgetType))) {
            QSignalBlocker blocker(combo);
            combo->setPlaceholderText(QStringLiteral("—"));
            combo->setCurrentIndex(-1);
            combo->setToolTip(tooltip);
        }
        return true;
    }
    if (binding.widgetType == QLatin1String("value_segmented")) {
        if (auto* segmented = qobject_cast<ValueSegmentedControl*>(binding.widget.data())) {
            segmented->setValue(QVariant());
            segmented->setToolTip(tooltip);
        }
        return true;
    }
    return true;
}

void PropertyEditorEngine::setWidgetValue(QWidget* widget, const QString& widgetType, const QVariant& value)
{
    if (widgetType == QStringLiteral("slider"))
    {
        // No blockSignals here: ParameterSlider::setValue is already silent, and
        // an in-session setValue must deliver interactiveEditEnd(committed=false)
        // so the engine cancels the interrupted session instead of orphaning it.
        if (auto* s = qobject_cast<ParameterSlider*>(widget)) {
            s->setMixedValue(false);
            s->setValue(value.toDouble());
        }
        return;
    }

    if (widgetType == QStringLiteral("int_spinbox"))
    {
        if (auto* sp = qobject_cast<QSpinBox*>(innerWidget(widget, widgetType))) {
            sp->blockSignals(true); sp->setValue(value.toInt()); sp->blockSignals(false);
        }
        return;
    }

    if (widgetType == QStringLiteral("checkbox"))
    {
        if (auto* c = qobject_cast<QCheckBox*>(widget)) {
            c->blockSignals(true);
            c->setTristate(false);
            c->setChecked(value.toBool());
            c->blockSignals(false);
        }
        return;
    }

    if (widgetType == QStringLiteral("combo"))
    {
        if (auto* combo = qobject_cast<QComboBox*>(innerWidget(widget, widgetType))) {
            combo->blockSignals(true);
            combo->setPlaceholderText(QString());
            int idx = combo->findData(value);
            if (idx < 0) {
                bool okValue = false;
                const double want = value.toDouble(&okValue);
                if (okValue) {
                    for (int i = 0; i < combo->count(); ++i) {
                        bool okData = false;
                        const double have = combo->itemData(i).toDouble(&okData);
                        if (okData && qFuzzyCompare(have + 1.0, want + 1.0)) {
                            idx = i;
                            break;
                        }
                    }
                }
            }
            if (idx >= 0) combo->setCurrentIndex(idx);
            combo->blockSignals(false);
        }
        return;
    }

    if (widgetType == QStringLiteral("segmented"))
    {
        const auto buttons = widget->findChildren<QToolButton*>();
        for (auto* button : buttons) {
            bool matches = button->property("segmentValue") == value;
            if (!matches) {
                bool valueOk = false;
                bool buttonOk = false;
                const double wanted = value.toDouble(&valueOk);
                const double actual = button->property("segmentValue").toDouble(&buttonOk);
                matches = valueOk && buttonOk && qFuzzyCompare(wanted + 1.0, actual + 1.0);
            }
            button->blockSignals(true);
            button->setChecked(matches);
            button->blockSignals(false);
        }
        return;
    }


    // Segment selection is a range question, not an equality one: the numeric
    // segment owns a whole sub-range, so the control resolves membership itself
    // rather than matching one stored value the way the fixed row does.
    if (widgetType == QStringLiteral("value_segmented"))
    {
        if (auto* control = qobject_cast<ValueSegmentedControl*>(widget))
            control->setValue(value);
        return;
    }

    if (widgetType == QStringLiteral("text"))
    {
        if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
            edit->blockSignals(true); edit->setText(value.toString());
            edit->blockSignals(false);
        }
        return;
    }

    if (widgetType == QStringLiteral("multiline_text"))
    {
        if (auto* edit = qobject_cast<QPlainTextEdit*>(widget)) {
            edit->blockSignals(true); edit->setPlainText(value.toString());
            edit->blockSignals(false);
        }
        return;
    }
}

QWidget* PropertyEditorEngine::innerWidget(QWidget* container, const QString& widgetType) const
{
    if (!container) return nullptr;
    if (widgetType == QStringLiteral("slider") || widgetType == QStringLiteral("checkbox"))
        return container;
    if (widgetType == QStringLiteral("int_spinbox"))
        if (auto* s = container->findChild<QSpinBox*>()) return s;
    if (widgetType == QStringLiteral("combo"))
        if (auto* c = container->findChild<QComboBox*>()) return c;
    if (widgetType == QStringLiteral("segmented"))
        return container;
    if (widgetType == QStringLiteral("value_segmented"))
        return container;
    if (widgetType == QStringLiteral("text"))
        if (auto* edit = qobject_cast<QLineEdit*>(container)) return edit;
    if (widgetType == QStringLiteral("multiline_text"))
        if (auto* edit = qobject_cast<QPlainTextEdit*>(container)) return edit;
    return container;
}

IComponent* PropertyEditorEngine::resolveComponent(ProjectItem* item) const
{
    return resolveComponentTyped(item, componentType_);
}

IComponent* PropertyEditorEngine::resolveComponentTyped(ProjectItem* item, ComponentType componentType)
{
    if (!item) return nullptr;
    switch (componentType)
    {
        case ComponentType::MixNode:     return item->comp<VMixNode>();
        case ComponentType::Audio:       return item->comp<VAudio>();
        case ComponentType::Selection:   return item->comp<VSelection>();
        case ComponentType::Bus:         return item->comp<VBus>();
        case ComponentType::Action:      return item->comp<VAction>();
        case ComponentType::Variable:    return item->comp<VVariable>();
        case ComponentType::Attenuation: return item->comp<VAttenuation>();
        case ComponentType::Spatial:     return item->comp<VSpatial>();
        case ComponentType::Track:       return item->comp<VTrack>();
        case ComponentType::Piece:       return item->comp<VPiece>();
        case ComponentType::Sound:       return item->comp<VSound>();
        default:                         return nullptr;
    }
}

QVariant PropertyEditorEngine::readField(const QString& field) const
{
    if (fieldGroup(field)) return groupFieldValue(field);
    if (const auto* property = PlayablePropertyAuthoringV1::fieldByName(field)) {
        ProjectItem* owner = nullptr;
        const auto locator = editablePlayableLocator(field, &owner);
        if (locator && owner) {
            if (PlayablePropertyAuthoringV1::isComponentStored(property->id))
                return playableBaseEditorValue(*owner, *locator, *property);
            const PlatformContext context =
                PlatformContextService::instance().context();
            if (context.selected.isEmpty()) {
                const auto state = PlayablePropertyAuthoringV1::readProperty(
                    *owner, *locator, property->id, std::nullopt);
                if (state) {
                    const auto value = editorValue(*property, state->value);
                    if (value) return *value;
                    return {};
                }
            } else {
                const PlatformFieldView view = platformFieldView(
                    *owner, *locator, property->id, context);
                if (!view.uniform)
                    return {};
                const auto value = editorValue(*property, view.value.value);
                if (value) return *value;
                return {};
            }
        }
    }
    if (currentItem_)
        for (const auto& d : schema_)
            if (d.field == field && d.getItem)
                return d.getItem(currentItem_);
    if (auto* comp = resolveComponent(currentItem_))
    {
        QVariant v = comp->getField(field);
        if (v.isValid()) return v;
    }
    if (currentItem_)
    {
        if (auto* mix = currentItem_->comp<VMixNode>())
        {
            QVariant v = mix->getField(field);
            if (v.isValid()) return v;
        }
        if (auto* spatial = currentItem_->comp<VSpatial>())
        {
            QVariant v = spatial->getField(field);
            if (v.isValid()) return v;
        }
        for (const auto& d : schema_)
            if (d.field == field && d.get && !d.set)
                return d.get(resolveComponent(currentItem_));
    }
    return {};
}

void PropertyEditorEngine::bindItem(ProjectItem* item)
{
    cancelGroupEdits(item == currentItem_);
    ProjectItem* previousItem = currentItem_;
    for (auto& binding : bindings_)
    {
        if (binding.parameterBinding && binding.parameterBinding->isEditing())
            binding.parameterBinding->cancelEdit();
        binding.editSession.reset();
    }
    currentItem_ = item;
    for (auto& [group, binding] : groupBindings_) binding.error.clear();
    refreshFieldGroups();

    if (item)
    {
        if (auto* c = resolveComponent(item))
            c->setHostItem(item);
        if (auto* mix = item->comp<VMixNode>())
            mix->setHostItem(item);
    }

    refreshDomains();

    updating_ = true;
    for (auto& b : bindings_)
    {
        tagFieldTopic(b, item);
        if (!b.widget) continue;
        if (fieldGroup(b.field)) continue;
        if (b.drawer) {
            QVariant drawerValue = readField(b.field);
            ProjectItem* owner = nullptr;
            const auto* property = PlayablePropertyAuthoringV1::fieldByName(b.field);
            const auto locator = property
                ? editablePlayableLocator(b.field, &owner) : std::nullopt;
            if (property && locator && owner && drawerValue.isValid() == false
                && property->referenceRole != PlayablePropertyReferenceRoleV1::Attenuation) {
                const PlatformContext context =
                    PlatformContextService::instance().context();
                if (!context.selected.isEmpty())
                    drawerValue = playableBaseEditorValue(*owner, *locator, *property);
            }
            b.drawer->bind(b.widget, drawerValue, item);
            if (property && locator && owner)
                applyDrawerMixedTooltip(b.widget, b.mixedTooltip, owner, *locator,
                    property->id);
        }
        else
        {
            QVariant v = readField(b.field);
            auto* multilineEdit = b.widgetType == QLatin1String("multiline_text")
                ? qobject_cast<QPlainTextEdit*>(b.widget.data())
                : nullptr;
            const bool preserveActiveEdit = multilineEdit
                && previousItem == item
                && multilineEdit->hasFocus();
            // Whose count a numeric segment should fall back to changes with the
            // bound object, so the context lands before the value does.
            if (auto* seg = qobject_cast<ValueSegmentedControl*>(b.widget.data()))
                seg->setContext(item
                    ? item->guid.toString(QUuid::WithoutBraces) + QLatin1Char('/') + b.field
                    : QString());
            const bool handled = PlayablePropertyAuthoringV1::fieldByName(b.field)
                && !preserveActiveEdit
                && applyPlayableFieldView(b);
            if (!handled && v.isValid() && !preserveActiveEdit)
                setWidgetValue(b.widget, b.widgetType, v);
        }
        if (b.decorator)
            b.decorator->bind(item);
        else
            for (IFieldDecorator* decorator : b.decorators)
                decorator->bind(item);
    }
    updating_ = false;

    for (const auto& s : sectionBindings_)
        if (s.provider && s.widget)
            s.provider->bind(s.widget, item);

    tagCustomFields(item);

    evalEditConditions();
}

void PropertyEditorEngine::refreshDomains()
{
    IComponent* component = currentItem_ ? resolveComponent(currentItem_) : nullptr;
    if (!component) return;

    for (const auto& b : bindings_)
    {
        if (!b.domain || !b.widget) continue;
        if (b.parameterBinding && b.parameterBinding->isEditing()) continue;
        const std::optional<FieldDomain> domain = b.domain(component);
        if (!domain) continue;
        if (b.drawer)
        {
            b.drawer->applyDomain(b.widget, *domain);
            continue;
        }
        auto* slider = qobject_cast<ParameterSlider*>(b.widget.data());
        if (!slider) continue;
        ParameterBinding* binding = slider->binding();
        binding->setQuantum(domain->quantum);
        binding->setRange(domain->min, domain->max);
        binding->setDecimals(domain->decimals);
        binding->setAnnotation(domain->annotation);
        binding->setDiscreteSelection(domain->discreteSelection);
    }
}

bool PropertyEditorEngine::refreshField(const QString& field)
{
    if (!currentItem_) return false;
    if (field == QLatin1String("fieldGroups") || fieldGroup(field)) {
        refreshFieldGroups();
        return !groupBindings_.empty();
    }

    for (const auto& binding : bindings_)
    {
        if (binding.field == field && binding.parameterBinding
            && binding.parameterBinding->isEditing())
            return true;
    }

    refreshDomains();

    bool handled = false;
    QString fieldName = field;
    std::optional<BindingAddress> addressFilter;
    const bool coarseBindings = field == QLatin1String("bindings");
    if (field.startsWith(QLatin1String("bindings."))) {
        const QStringList parts = field.split(QLatin1Char('.'));
        if (parts.size() >= 3) {
            const auto hostClass = BindingContractSemantics::parseHostClass(parts.at(1));
            bool ok = false;
            const int slotId = parts.at(2).toInt(&ok);
            if (hostClass && ok && slotId >= 1 && slotId <= 16) {
                BindingAddress address;
                address.hostClass = *hostClass;
                address.slotId = static_cast<uint8_t>(slotId);
                if (parts.size() >= 4) {
                    bool pathOk = false;
                    const int pathIndex = parts.at(3).toInt(&pathOk);
                    if (pathOk && pathIndex >= 0)
                        address.path = BindingPath{static_cast<uint32_t>(pathIndex)};
                }
                addressFilter = address;
            }
        }
        fieldName.clear();
    }
    for (auto& b : bindings_)
    {
        const bool fieldMatch = !fieldName.isEmpty() && b.field == fieldName;
        const bool addressMatch = addressFilter && b.bindingAddress
            && *b.bindingAddress == *addressFilter;
        const bool coarseHang = coarseBindings && b.bindingAddress.has_value();
        if ((!fieldMatch && !addressMatch && !coarseHang) || !b.widget) continue;
        updating_ = true;
        if (b.drawer) {
            QVariant drawerValue = readField(b.field);
            ProjectItem* owner = nullptr;
            const auto* property = PlayablePropertyAuthoringV1::fieldByName(b.field);
            const auto locator = property
                ? editablePlayableLocator(b.field, &owner) : std::nullopt;
            if (property && locator && owner && drawerValue.isValid() == false
                && property->referenceRole != PlayablePropertyReferenceRoleV1::Attenuation) {
                const PlatformContext context =
                    PlatformContextService::instance().context();
                if (!context.selected.isEmpty())
                    drawerValue = playableBaseEditorValue(*owner, *locator, *property);
            }
            b.drawer->bind(b.widget, drawerValue, currentItem_);
            if (property && locator && owner)
                applyDrawerMixedTooltip(b.widget, b.mixedTooltip, owner, *locator,
                    property->id);
        }
        else
        {
            QVariant v = readField(b.field);
            auto* multilineEdit = b.widgetType == QLatin1String("multiline_text")
                ? qobject_cast<QPlainTextEdit*>(b.widget.data())
                : nullptr;
            const bool playableHandled = PlayablePropertyAuthoringV1::fieldByName(b.field)
                && (!multilineEdit || !multilineEdit->hasFocus())
                && applyPlayableFieldView(b);
            if (!playableHandled && v.isValid()
                && (!multilineEdit || !multilineEdit->hasFocus()))
                setWidgetValue(b.widget, b.widgetType, v);
        }
        if (b.decorator)
            b.decorator->bind(currentItem_);
        else
            for (IFieldDecorator* decorator : b.decorators)
                decorator->bind(currentItem_);
        updating_ = false;
        handled = true;
        if (!addressFilter && !coarseBindings) break;
    }

    for (const auto& s : sectionBindings_) {
        if (!s.provider || !s.widget) continue;
        if (s.provider->refresh(s.widget, currentItem_, field))
            handled = true;
    }

    if (handled)
        evalEditConditions();
    return handled;
}

bool PropertyEditorEngine::playablePropertyEventRelated(
    ProjectItem* owner, const QString& field) const
{
    if (owner && currentItem_
        && (field == QLatin1String("fieldGroups") || fieldGroup(field))) {
        for (auto* ancestor = currentItem_; ancestor; ancestor = ancestor->parent)
            if (ancestor == owner) return true;
        return false;
    }
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    if (!owner || !currentItem_ || !property)
        return false;
    if (PlayablePropertyAuthoringV1::isComponentStored(property->id)) {
        const auto* project = ProjectManager::instance().currentProject();
        if (!project) return owner == currentItem_;
        const auto index = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*project);
        if (!index || !index->valid) return false;
        for (auto* ancestor = currentItem_; ancestor;) {
            if (ancestor == owner) return true;
            const auto parent = index->parentDefinition.find(ancestor);
            ancestor = parent == index->parentDefinition.end() ? nullptr : parent->second;
        }
        return false;
    }
    ProjectItem* editableOwner = nullptr;
    const auto locator = editablePlayableLocator(field, &editableOwner);
    return locator && editableOwner == owner;
}

void PropertyEditorEngine::evalEditConditions()
{
    for (const auto& b : bindings_)
    {
        if (!b.widget) continue;
        const bool editable = canEditField(b);
        if (auto* ref = qobject_cast<ObjectRefWidget*>(b.widget.data()))
            ref->setReadOnly(!editable);
        else if (b.type == PropType::List)
            ListFieldDrawer::setWidgetReadOnly(b.widget, !editable);
        else
            b.widget->setEnabled(editable);
        if (auto* strip = dynamic_cast<InspectorSlotStrip*>(b.decorator))
            strip->setEnabled(editable);
        if (fieldGroup(b.field)) b.widget->setToolTip(b.mixedTooltip);
        if (b.topicHost)
            if (auto* label = b.topicHost->findChild<QLabel*>(QStringLiteral("label_") + b.field))
                label->setEnabled(editable);
    }
}

bool PropertyEditorEngine::evalCondition(const QString& expr,
                                         const std::function<QVariant(const QString&)>& get)
{
    const QString e = expr.trimmed();
    if (e.isEmpty()) return true;

    const QStringList clauses = e.split(QStringLiteral("&&"), Qt::SkipEmptyParts);
    for (const QString& raw : clauses)
    {
        const QString clause = raw.trimmed();
        bool neq = false;
        int op = clause.indexOf(QStringLiteral("=="));
        if (op < 0) { op = clause.indexOf(QStringLiteral("!=")); neq = (op >= 0); }
        if (op < 0) return false;

        const QString lhsName = clause.left(op).trimmed();
        const QString rhsLit  = clause.mid(op + 2).trimmed();
        if (lhsName.isEmpty() || rhsLit.isEmpty()) return false;

        const QVariant lhs = get(lhsName);
        if (!lhs.isValid()) return false;
        bool equal;
        if (rhsLit == QStringLiteral("true") || rhsLit == QStringLiteral("false"))
        {
            equal = (lhs.toBool() == (rhsLit == QStringLiteral("true")));
        }
        else
        {
            bool okL = false, okR = false;
            const double dr = rhsLit.toDouble(&okR);
            const double dl = lhs.toDouble(&okL);
            equal = (okL && okR) ? qFuzzyCompare(dl + 1.0, dr + 1.0)
                                 : (lhs.toString() == rhsLit);
        }
        if ((neq ? !equal : equal) == false) return false;
    }
    return true;
}

}
