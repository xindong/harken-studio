#include "BindingFieldEditor.h"
#include "BindingAddMenu.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"

#include "Core/ActionManager.h"
#include "Core/BindingEditing.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VVariable.h"
#include "Core/CurveSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace VNX {

namespace {

QString addressLabel(const BindingAddress& address)
{
    QString label = BindingSemantics::slotName(address.slotId);
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    return label;
}

QString editText(const BindingAddress& address, const QString& verb)
{
    return QObject::tr("%1 %2 Binding").arg(verb, addressLabel(address));
}

class BindingCurveThumbnail final : public QWidget
{
public:
    explicit BindingCurveThumbnail(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("bindingCurveThumb"));
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        setToolTip(tr("Edit Curve"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("preview"));
    }

    void setBinding(const ParameterBindingDef& binding)
    {
        binding_ = binding;
        const ProjectItem* item = ProjectManager::instance().findItemByGuid(binding.variable);
        const auto* variable = item ? item->comp<VVariable>() : nullptr;
        kind_ = variable ? variable->kind : VariableKind::Smooth;
        inputMin_ = variable ? variable->rangeMin : 0.0f;
        inputMax_ = variable ? variable->rangeMax : 1.0f;
        yMin_ = std::min(binding.outputMin, binding.outputMax);
        yMax_ = std::max(binding.outputMin, binding.outputMax);
        const ProjectItemType hostType = binding.address.hostClass == BindingHostClass::Bus
            ? ProjectItemType::Bus : ProjectItemType::Sound;
        const auto contract = BindingContractSemantics::derive(binding.address.slotId, hostType);
        clipToPlot_ = contract.outputUnit == BindingOutputUnit::Cent;
        volume_ = contract.outputUnit
            == BindingOutputUnit::Decibel;
        if (volume_ || clipToPlot_) {
            yMin_ = contract.legalOutputRange.min;
            yMax_ = contract.legalOutputRange.max;
        }
        if (yMin_ == yMax_) {
            yMin_ -= 1.0f;
            yMax_ += 1.0f;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QtControlSkin::paintFieldFrame(painter, this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto& theme = ThemeManager::instance();
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (binding_.curve.size() < 2) return;
        const QRectF area = frame.adjusted(2.0, 2.5, -2.0, -2.5);
        QPolygonF points;
        float lowest = std::numeric_limits<float>::max();
        float highest = std::numeric_limits<float>::lowest();
        constexpr int samples = 32;
        for (int i = 0; i <= samples; ++i) {
            const float nx = static_cast<float>(i) / samples;
            const float x = inputMin_ + nx * (inputMax_ - inputMin_);
            const float y = BindingSemantics::contribution(
                binding_, kind_, x, inputMin_, inputMax_);
            lowest = std::min(lowest, y);
            highest = std::max(highest, y);
            const double normalized = curveAxisNormalizedY(
                volume_ ? CurveAxisScale::MirroredAmplitudeDb : CurveAxisScale::LinearValue,
                yMin_, yMax_, y);
            const double ny = clipToPlot_ ? normalized : qBound(0.0, normalized, 1.0);
            points.append(QPointF(area.left() + nx * area.width(),
                                  area.bottom() - ny * area.height()));
        }
        if (clipToPlot_) {
            if (highest < yMin_ || lowest > yMax_) return;
            painter.setClipRect(area);
        }
        painter.setPen(QPen(
            theme.snapshot()->domains.curve.colors.parameterBinding, 1.2));
        painter.drawPolyline(points);
    }

private:
    ParameterBindingDef binding_;
    VariableKind kind_ = VariableKind::Smooth;
    bool volume_ = false;
    bool clipToPlot_ = false;
    float inputMin_ = 0.0f;
    float inputMax_ = 1.0f;
    float yMin_ = 0.0f;
    float yMax_ = 1.0f;
};

}

BindingFieldEditor::BindingFieldEditor(QWidget* parent)
    : QFrame(parent)
{
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::StrongFocus);
    QtControlSkin::setSemanticProperty(this, "surfaceRole", QStringLiteral("secondary"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    table_ = new TableWidget(this);
    table_->setSelectionClearHandler([this] { selectRow(-1); });
    table_->setRowSelectionHandler([this](QWidget* row) {
        selectRow(row->property("rowIndex").toInt());
    });
    table_->setColumns(QStringLiteral("Binding"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0},
        {QStringLiteral("Variable"), tr("Variable"), 600},
        {QStringLiteral("Curve"), tr("Curve"), 400}
    });
    table_->setRowMoveHandler([this](int from, int to) {
        selectRow(from);
        ActionManager::instance().execute(QStringLiteral("reorder_bindings"),
            BindingEditing::reorderArgs(item_, address_, from, to,
                                        editText(address_, tr("Reorder"))),
            ActionSource::GUI, this);
    }, [this](int from, int to) {
        return item_ && from >= 0 && to >= 0 && from < static_cast<int>(entries_.size())
            && to < static_cast<int>(entries_.size());
    });
    rows_ = table_->rowsWidget();
    auto* footer = table_->footer();
    add_ = footer->addButton();
    add_->setObjectName(QStringLiteral("bindingAdd"));
    addMenu_ = new BindingAddMenu(add_);
    connect(addMenu_, &BindingAddMenu::bindingAdded, this, [this](harkenstudio::ObjectId id) {
        selectedId_ = id;
    });
    remove_ = footer->removeButton();
    remove_->setObjectName(QStringLiteral("bindingRemoveSelected"));
    layout->addWidget(table_);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(add_, &QToolButton::clicked, this, &BindingFieldEditor::showAddMenu);
    connect(remove_, &QToolButton::clicked, this, &BindingFieldEditor::removeSelected);
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* changed, const QString& property, const QObject*) {
        if (!item_) return;
        if (property == QLatin1String("bindings")
            || property == BindingEditing::changeKey(address_)) {
            if (changed != item_) return;
            QTimer::singleShot(0, this, &BindingFieldEditor::reload);
        } else if (changed && changed->type == ProjectItem::Type::Variable)
            QTimer::singleShot(0, this, &BindingFieldEditor::refreshCurves);
    });
    connect(&manager, &ProjectManager::itemCreated, this,
            [this](ProjectItem* created) {
        if (created && created->type == ProjectItem::Type::Variable)
            QTimer::singleShot(0, this, &BindingFieldEditor::reload);
    });
    connect(&manager, &ProjectManager::itemDeleted, this,
            [this](ProjectItem* deleted) {
        if (deleted == item_)
            bind(nullptr, address_);
        else
            QTimer::singleShot(0, this, &BindingFieldEditor::reload);
    });
    connect(&manager, &ProjectManager::projectClosed, this,
            [this] { bind(nullptr, address_); });
}

BindingFieldEditor::~BindingFieldEditor() = default;

void BindingFieldEditor::bind(ProjectItem* item, const BindingAddress& address)
{
    if (item != item_ || address != address_) {
        table_->clearSelection();
    }
    item_ = item;
    address_ = address;
    addMenu_->bind(item, address);
    setObjectName(QStringLiteral("bindingFieldEditor_")
                  + BindingSemantics::slotName(address.slotId));
    setProperty("bindingAddress", BindingSemantics::addressKey(address));
    reload();
}

void BindingFieldEditor::selectBinding(const QUuid& owner, harkenstudio::ObjectId stableId)
{
    if (!item_ || owner != item_->guid) return;
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
        if (entries_[i].binding.runtimeId() == stableId) {
            selectRow(i);
            return;
        }
}

void BindingFieldEditor::showAddMenu()
{
    addMenu_->show();
}

void BindingFieldEditor::reload()
{
    auto* layout = static_cast<QVBoxLayout*>(rows_->layout());
    while (QLayoutItem* child = layout->takeAt(0)) {
        delete child->widget();
        delete child;
    }
    entries_.clear();
    if (const auto* mix = BindingEditing::ledgerHost(item_, address_))
        for (const auto* binding : mix->bindings.findAll(address_))
            entries_.push_back({*binding, {}});
    int restored = -1;
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        entries_[i].widget = buildRow(i);
        layout->addWidget(entries_[i].widget);
        if (entries_[i].binding.runtimeId() == selectedId_)
            restored = i;
    }
    add_->setEnabled(item_ != nullptr);
    selectRow(restored);
    resizeToRows();
}

QWidget* BindingFieldEditor::buildRow(int index)
{
    const Row& entry = entries_[index];
    auto* row = new QWidget(rows_);
    row->setObjectName(QStringLiteral("listRow"));
    row->setAttribute(Qt::WA_StyledBackground);
    row->setProperty("rowIndex", index);
    row->setProperty("bindingStableId", QVariant::fromValue(entry.binding.runtimeId()));
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* cells = new QHBoxLayout();
    cells->setSpacing(0);
    auto* handle = table_->createRowNumber(row, index);
    handle->setObjectName(QStringLiteral("bindingDragHandle"));
    handle->setProperty("tableRowNumber", true);
    handle->setCursor(Qt::SizeVerCursor);
    handle->setProperty("rowIndex", index);
    handle->installEventFilter(this);
    cells->addWidget(handle);
    auto* variableCell = table_->createCell(row);
    auto* variable = new ObjectRefWidget(variableCell);
    variable->setObjectName(QStringLiteral("bindingVariableRef"));
    variable->setFilterType(ProjectItem::Type::Variable);
    variable->setSelectorTitle(tr("Variable"));
    variable->setCompact(true);
    variable->setCompactPickable(true);
    variable->setClearSupport(false);
    const auto* source = ProjectManager::instance().findItemByGuid(entry.binding.variable);
    if (source)
        variable->setCurrentObjectID(source->objectID);
    else
        variable->setUnresolvedReference(entry.binding.variable.toString(QUuid::WithoutBraces));
    variable->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    variableCell->addWidget(variable);
    cells->addWidget(variableCell);
    auto* thumbnailCell = table_->createCell(row);
    auto* thumbnail = new BindingCurveThumbnail(thumbnailCell);
    thumbnail->setBinding(entry.binding);
    thumbnail->setProperty("rowIndex", index);
    thumbnail->installEventFilter(this);
    thumbnailCell->addWidget(thumbnail);
    cells->addWidget(thumbnailCell);
    layout->addLayout(cells);
    connect(variable, &ObjectRefWidget::objectChanged, this,
            [this, index](uint32_t id) { changeVariable(index, id); });
    table_->registerRow(row, {handle, variableCell, thumbnailCell});
    row->installEventFilter(this);
    return row;
}

void BindingFieldEditor::selectRow(int index)
{
    selected_ = index >= 0 && index < static_cast<int>(entries_.size()) ? index : -1;
    selectedId_ = selected_ >= 0 ? entries_[selected_].binding.runtimeId() : harkenstudio::ObjectId{};
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
        if (entries_[i].widget)
            QtControlSkin::setSemanticProperty(entries_[i].widget, "selected", i == selected_);
    remove_->setEnabled(selected_ >= 0);
    emit curveSelectionChanged(selected_ >= 0
        ? CurveSelection::binding(item_->guid, address_, selectedId_) : CurveSelection{});
}

void BindingFieldEditor::refreshCurves()
{
    for (const Row& row : entries_) {
        if (!row.widget) continue;
        if (auto* widget = row.widget->findChild<QWidget*>(QStringLiteral("bindingCurveThumb")))
            static_cast<BindingCurveThumbnail*>(widget)->setBinding(row.binding);
    }
}

void BindingFieldEditor::changeVariable(int index, uint32_t objectId)
{
    if (!item_ || index < 0 || index >= static_cast<int>(entries_.size())) return;
    const Row row = entries_[index];
    const auto* variable = ProjectManager::instance().findItemByObjectID(objectId);
    if (!variable || !variable->comp<VVariable>()) return;
    const auto* mix = BindingEditing::ledgerHost(item_, address_);
    const auto* stored = mix ? mix->bindings.find(row.binding.runtimeId()) : nullptr;
    if (!stored) return;
    ParameterBindingDef binding = *stored;
    binding.variable = variable->guid;
    ActionManager::instance().execute(QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(item_, binding, editText(address_, tr("Change"))),
        ActionSource::GUI, this);
}

void BindingFieldEditor::removeSelected()
{
    if (!item_ || selected_ < 0) return;
    table_->removeSelectedRow([this] {
        return ActionManager::instance().execute(QStringLiteral("remove_binding"),
            BindingEditing::removeArgs(item_, selectedId_, editText(address_, tr("Remove"))),
            ActionSource::GUI, this).success;
    });
}

void BindingFieldEditor::navigateTo(const QUuid& guid)
{
    emit navigateRequested(guid);
}

bool BindingFieldEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::MouseButtonPress)
        return QFrame::eventFilter(watched, event);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (!watched->property("rowIndex").isValid() || mouse->button() != Qt::LeftButton)
        return QFrame::eventFilter(watched, event);
    const int index = watched->property("rowIndex").toInt();
    selectRow(index);
    if (watched->objectName() == QLatin1String("bindingCurveThumb")) {
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void BindingFieldEditor::resizeToRows()
{
    ensurePolished();
    layout()->activate();
    table_->refreshLayout();
    updateGeometry();
}

void BindingFieldEditor::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    resizeToRows();
}

}
