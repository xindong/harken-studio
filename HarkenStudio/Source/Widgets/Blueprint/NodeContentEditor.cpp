#include "NodeContentEditor.h"

#include "Core/ContextInfo.h"

#include "Core/Components/VAudio.h"
#include "Core/Components/VMixNode.h"
#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ValueSegmentedControl.h"

#include <QEvent>
#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleOptionSpinBox>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <cmath>

namespace VNX {
namespace {

constexpr std::array kFields{NodeContentField::Volume, NodeContentField::Pitch,
    NodeContentField::OffsetOfStart, NodeContentField::Loop, NodeContentField::OutputVolume,
    NodeContentField::SendVolume};

double fieldValue(const NodeContentInfo& info, NodeContentField field)
{
    switch (field) {
    case NodeContentField::Volume: return info.volume.value_or(0.0f);
    case NodeContentField::Pitch: return info.pitch.value_or(0.0f);
    case NodeContentField::OutputVolume: return info.outputVolume.value_or(0.0f);
    case NodeContentField::SendVolume: return VMixNode::linearToDb(info.sendVolume.value_or(1.0f));
    case NodeContentField::OffsetOfStart: return info.offsetOfStart.value_or(0.0);
    case NodeContentField::Loop: return info.loopCount.value_or(1);
    case NodeContentField::Waveform: break;
    }
    return 0.0;
}

double authoredFieldValue(NodeContentField field, double value)
{
    return field == NodeContentField::SendVolume
        ? VMixNode::dbToLinear(static_cast<float>(value)) : value;
}

}

PropertyDescriptor nodeContentProperty(NodeContentField field)
{
    if (field == NodeContentField::OffsetOfStart) {
        PropertyDescriptor descriptor;
        descriptor.field = QStringLiteral("scheduleOffsetSec");
        descriptor.type = PropType::Float;
        descriptor.label = QStringLiteral("Offset of Start");
        descriptor.unit = QStringLiteral(" s");
        descriptor.min = 0.0;
        descriptor.max = 100.0;
        descriptor.decimals = 3;
        return descriptor;
    }
    if (field == NodeContentField::SendVolume) {
        const auto& schema = VMixNode::schema();
        const auto& sends = *std::find_if(schema.begin(), schema.end(),
            [](const auto& descriptor) { return descriptor.field == QLatin1String("sends"); });
        auto descriptor = *std::find_if(sends.elementSchema.begin(), sends.elementSchema.end(),
            [](const auto& entry) { return entry.field == QLatin1String("sendLevel"); });
        descriptor.label = QStringLiteral("Send Volume");
        return descriptor;
    }
    const auto& schema = field == NodeContentField::Loop ? VAudio::schema() : VMixNode::schema();
    const QString name = field == NodeContentField::Volume ? QStringLiteral("volume")
        : field == NodeContentField::Pitch ? QStringLiteral("pitch")
        : field == NodeContentField::Loop ? QStringLiteral("loopCount")
        : QStringLiteral("outputBusVolume");
    return *std::find_if(schema.begin(), schema.end(),
        [&](const auto& descriptor) { return descriptor.field == name; });
}

struct NodeContentEditor::Row
{
    NodeContentField field;
    PropertyDescriptor property;
    QLabel* label = nullptr;
    QLabel* value = nullptr;
    ParameterControlHost* slider = nullptr;
    ParameterBinding* binding = nullptr;
    DecimalSpinBox* editor = nullptr;
    QStackedWidget* valueStack = nullptr;
    ValueSegmentedControl* loop = nullptr;
    QWidget* loopHost = nullptr;
    bool editingValue = false;
};

NodeContentEditor::NodeContentEditor(const NodeContentInfo& info, NodeContentFields fields,
                                     int columnWidth, QWidget* parent)
    : QWidget(parent), fields_(fields), labelColumnWidth_(columnWidth)
{
    setObjectName(QStringLiteral("NodeContentEditor"));
    setProperty("sliderStyle", QStringLiteral("compact"));
    setAttribute(Qt::WA_TranslucentBackground);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(0);
    for (auto field : kFields) {
        if (!info.fields().testFlag(field)) continue;
        auto row = std::make_unique<Row>();
        row->field = field;
        row->property = nodeContentProperty(field);
        row->label = new QLabel(row->property.label, this);
        row->label->setProperty("nodeContentRole", QStringLiteral("label"));
        row->label->setFont(FontManager::instance().getFont(FontRole::Label, 9));
        row->label->setFixedWidth(columnWidth);
        row->label->setToolTip(row->label->text());
        row->valueStack = new QStackedWidget(this);
        row->valueStack->setFixedWidth(kValueWidth);
        row->value = new QLabel(row->valueStack);
        row->value->setProperty("nodeContentRole", QStringLiteral("value"));
        row->value->setObjectName(QStringLiteral("nodeValue_") + row->property.field);
        row->value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->value->setFont(FontManager::instance().getFont(FontRole::Label, 9, QFont::DemiBold));
        row->valueStack->addWidget(row->value);

        if (field == NodeContentField::Loop) {
            row->loopHost = new QWidget(this);
            auto* layout = new QHBoxLayout(row->loopHost);
            layout->setContentsMargins(0, 0, 0, 0);
            row->loop = new ValueSegmentedControl(row->loopHost);
            row->loop->setObjectName(QStringLiteral("nodeEditor_loopCount"));
            row->loop->setSegments(row->property.segments);
            row->loop->installEventFilter(this);
            for (auto* button : row->loop->findChildren<QToolButton*>())
                button->installEventFilter(this);
            layout->addWidget(row->loop);
            row->loopHost->hide();
            row->value->installEventFilter(this);
            Row* entry = row.get();
            connect(row->loop, &ValueSegmentedControl::segmentPicked, this,
                [this, field, entry](const QVariant& value) {
                    entry->loopHost->hide();
                    entry->valueStack->show();
                    emit valueCommitted(field, value);
                    emit editingClosed();
                });
            connect(row->loop, &ValueSegmentedControl::numericEditBegan, this,
                [this, field]() { emit editBegan(field); });
            connect(row->loop, &ValueSegmentedControl::numericPreviewed, this,
                [this, field](const QVariant& value) { emit editPreviewed(field, value); });
            connect(row->loop, &ValueSegmentedControl::numericEditEnded, this,
                [this, field, entry](bool committed) {
                    entry->loopHost->hide();
                    entry->valueStack->show();
                    emit editEnded(field, committed);
                    emit editingClosed();
                });
        } else {
            row->slider = new ParameterControlHost(row->property.label,
                ParameterPresenterContext::InspectorFloat,
                ParameterPresenterId::HorizontalSlider, this);
            row->slider->setObjectName(QStringLiteral("nodeSlider_") + row->property.field);
            row->slider->setFixedWidth(kSliderWidth);
            row->binding = row->slider->binding();
            row->binding->setRange(row->property.min.toDouble(), row->property.max.toDouble());
            row->binding->setDecimals(row->property.decimals);
            row->binding->setQuantum(std::pow(10.0, -row->property.decimals));
            row->binding->setDefaultValue(row->property.def.isValid() ? row->property.def.toDouble() : 0.0);
            row->binding->setUnit(row->property.unit);
            row->editor = new DecimalSpinBox(row->valueStack);
            row->editor->setObjectName(QStringLiteral("nodeEditor_") + row->property.field);
            row->editor->setDecimals(row->property.decimals);
            row->editor->setRange(row->binding->minimum(), row->binding->maximum());
            row->editor->setSingleStep(std::pow(10.0, -row->property.decimals));
            row->editor->setKeyboardTracking(false);
            row->editor->setButtonSymbols(QAbstractSpinBox::NoButtons);
            row->editor->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->editor->setFont(row->value->font());
            row->editor->setProperty("fieldRole", QStringLiteral("inspectorValue"));
            row->valueStack->addWidget(row->editor);
            row->value->installEventFilter(this);
            row->editor->installEventFilter(this);
            Row* entry = row.get();
            connect(row->binding, &ParameterBinding::editBegan, this,
                [this, field](double) { emit editBegan(field); });
            connect(row->binding, &ParameterBinding::editPreviewed, this,
                [this, field](double value) { emit editPreviewed(field, authoredFieldValue(field, value)); });
            connect(row->binding, &ParameterBinding::editEnded, this,
                [this, field](double, double, bool committed) {
                    updateMetrics();
                    emit editEnded(field, committed);
                });
            connect(row->binding, &ParameterBinding::valueCommitted, this,
                [this, field](double value) { emit valueCommitted(field, authoredFieldValue(field, value)); });
            connect(row->binding, &ParameterBinding::valueUpdated, this,
                [this, entry](double) { refreshValue(*entry); });
            connect(row->editor, &QDoubleSpinBox::valueChanged, this,
                [entry](double value) {
                    if (entry->editingValue) entry->binding->previewEdit(value);
                });
            connect(row->editor, &QDoubleSpinBox::editingFinished, this,
                [this, entry]() { finishValueEdit(*entry, true); });
            connect(row->editor->findChild<QLineEdit*>(), &QLineEdit::textChanged,
                this, &NodeContentEditor::updateMetrics);
        }

        const int index = static_cast<int>(rows_.size());
        grid->addWidget(row->label, index, 0);
        if (row->slider) grid->addWidget(row->slider, index, 1);
        grid->addWidget(row->valueStack, index, 2);
        if (row->loopHost) grid->addWidget(row->loopHost, index, 1, 1, 2);
        rows_.push_back(std::move(row));
    }
    grid->setColumnMinimumWidth(1, kSliderWidth);
    setValues(info);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
        this, &NodeContentEditor::updateMetrics);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* focused) {
        if (focused && focused != this && !isAncestorOf(focused) && isEditing())
            finishEditing(true);
    });
    setFields(fields);
}

NodeContentEditor::~NodeContentEditor() = default;

int NodeContentEditor::labelWidth(NodeContentFields fields)
{
    QLabel label;
    label.setFont(FontManager::instance().getFont(FontRole::Label, 9));
    label.ensurePolished();
    int width = 0;
    for (auto field : kFields) {
        if (!fields.testFlag(field)) continue;
        label.setText(nodeContentProperty(field).label);
        width = std::max(width, label.sizeHint().width());
    }
    return width;
}

ParameterBinding* NodeContentEditor::binding(NodeContentField field) const
{
    for (const auto& row : rows_)
        if (row->field == field) return row->binding;
    return nullptr;
}

std::optional<NodeContentField> NodeContentEditor::fieldAt(const QPoint& position) const
{
    for (const auto& row : rows_) {
        if (!fields_.testFlag(row->field))
            continue;
        const QWidget* candidates[] = {row->label, row->valueStack, row->slider, row->loopHost};
        QRect band;
        bool covered = false;
        for (const QWidget* widget : candidates) {
            if (!widget || widget->isHidden())
                continue;
            band = covered ? band.united(widget->geometry()) : widget->geometry();
            covered = true;
        }
        if (!covered)
            continue;
        band = QRect(0, band.top(), width(), band.height());
        if (band.contains(position))
            return row->field;
    }
    return std::nullopt;
}

void NodeContentEditor::setFieldTopic(NodeContentField field, const ContextTopic& topic)
{
    for (const auto& row : rows_) {
        if (row->field != field)
            continue;
        QWidget* widgets[] = {
            row->label, row->value, row->valueStack, row->editor,
            row->slider, row->loop, row->loopHost
        };
        for (QWidget* widget : widgets)
            ContextInfo::tag(widget, topic);
        return;
    }
}

void NodeContentEditor::setFields(NodeContentFields fields)
{
    finishEditing(true);
    fields_ = fields;
    for (size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        const bool visible = fields.testFlag(row->field);
        row->label->setVisible(visible);
        row->valueStack->setVisible(visible);
        if (row->slider) row->slider->setVisible(visible);
    }
    updateMetrics();
}

void NodeContentEditor::setValues(const NodeContentInfo& info)
{
    for (auto& row : rows_) {
        if (row->binding) {
            if (!row->binding->isEditing()) row->binding->setValue(fieldValue(info, row->field));
        } else if (row->field == NodeContentField::Loop) {
            const auto count = info.loopCount.value_or(1);
            row->value->setText(count == 0 ? QString(QChar(0x221E)) : QString::number(count));
            if (row->loop && !row->loop->isEditing()) row->loop->setValue(count);
        } else {
            const double value = fieldValue(info, row->field);
            row->value->setText(value <= -96.0 ? QStringLiteral("-inf")
                : formatDecimal(value, row->property.decimals));
        }
        refreshValue(*row);
    }
}

void NodeContentEditor::setContext(const QString& context)
{
    for (const auto& row : rows_)
        if (row->loop) row->loop->setContext(context + QStringLiteral("/loopCount"));
}

void NodeContentEditor::setRouteGainMode(bool enabled)
{
    if (routeGainMode_ == enabled) return;
    finishEditing(true);
    routeGainMode_ = enabled;
    auto* grid = qobject_cast<QGridLayout*>(layout());
    for (int row = 0; row < grid->rowCount(); ++row) grid->setRowMinimumHeight(row, 0);
    for (size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        const int position = static_cast<int>(index) * (enabled ? 2 : 1);
        grid->addWidget(row->label, position, 0);
        grid->addWidget(row->valueStack, position, 2);
        if (row->slider)
            grid->addWidget(row->slider, position + (enabled ? 1 : 0), enabled ? 0 : 1, 1, enabled ? 3 : 1);
        if (row->loopHost) grid->addWidget(row->loopHost, position, 1, 1, 2);
    }
    updateMetrics();
}

bool NodeContentEditor::isDragHandleAt(const QPoint& position) const
{
    const auto* child = childAt(position);
    if (qobject_cast<const QSlider*>(child)) return false;
    for (const auto& row : rows_) {
        if (row->editingValue && (child == row->editor || row->editor->isAncestorOf(child))) return false;
        if (row->loopHost && !row->loopHost->isHidden()
            && (child == row->loopHost || row->loopHost->isAncestorOf(child))) return false;
    }
    return true;
}

bool NodeContentEditor::beginValueEditingAt(const QPoint& position)
{
    for (const auto& row : rows_) {
        if (row->editingValue || row->valueStack->isHidden()
            || !row->valueStack->geometry().contains(position)) continue;
        if (row->loopHost) {
            row->valueStack->hide();
            row->loopHost->show();
            row->loop->setFocus(Qt::MouseFocusReason);
        } else {
            const QSignalBlocker blocker(row->editor);
            row->editor->setValue(row->binding->value());
            row->editingValue = true;
            row->binding->beginEdit();
            row->valueStack->setCurrentWidget(row->editor);
            row->editor->setFocus(Qt::MouseFocusReason);
            row->editor->selectAll();
        }
        return true;
    }
    return false;
}

void NodeContentEditor::refreshValue(Row& row)
{
    if (row.binding)
        row.value->setText(row.property.unit.trimmed() == QLatin1String("dB")
            && row.binding->value() <= -96.0 ? QStringLiteral("-inf")
            : formatDecimal(row.binding->value(), row.property.decimals));
    const QString tip = QStringLiteral("%1 %2").arg(row.value->text(), row.property.unit.trimmed()).trimmed();
    row.value->setToolTip(tip);
    if (row.editor) row.editor->setToolTip(tip);
    updateMetrics();
}

void NodeContentEditor::updateMetrics()
{
    int valueWidth = kValueWidth;
    int rowHeight = 20;
    int visibleRows = 0;
    for (const auto& row : rows_) {
        if (!fields_.testFlag(row->field)) continue;
        ++visibleRows;
        row->label->ensurePolished();
        row->value->ensurePolished();
        rowHeight = std::max(rowHeight, row->label->sizeHint().height() + 2);
        valueWidth = std::max(valueWidth, row->value->sizeHint().width() + 8);
        if (row->editor) {
            row->editor->ensurePolished();
            const QString text = row->editingValue ? row->editor->text()
                : formatDecimal(row->binding->value(), row->property.decimals, row->editor->locale());
            QStyleOptionSpinBox option;
            option.initFrom(row->editor);
            option.buttonSymbols = row->editor->buttonSymbols();
            option.frame = row->editor->hasFrame();
            const QSize textSize(row->editor->fontMetrics().horizontalAdvance(text) + 2,
                row->editor->fontMetrics().height());
            valueWidth = std::max(valueWidth,
                row->editor->style()->sizeFromContents(QStyle::CT_SpinBox, &option,
                    textSize, row->editor).width());
        }
    }
    if (isEditing()) valueWidth = std::max(valueWidth, valueColumnWidth_);
    valueColumnWidth_ = valueWidth;
    auto* grid = qobject_cast<QGridLayout*>(layout());
    const int spacing = routeGainMode_ ? 6 : 8;
    const int columnWidth = routeGainMode_
        ? labelWidth(NodeContentField::Volume)
        : labelColumnWidth_;
    const int contentWidth = routeGainMode_
        ? std::max(kRouteGainMinimumWidth, columnWidth + valueWidth + 2 * spacing)
        : columnWidth + kSliderWidth + valueWidth + 2 * spacing;
    const int sliderWidth = routeGainMode_ ? contentWidth : kSliderWidth;
    const int sliderHeight = routeGainMode_ ? 12 : rowHeight;
    grid->setHorizontalSpacing(spacing);
    grid->setColumnMinimumWidth(1, routeGainMode_ ? 0 : sliderWidth);
    grid->setColumnStretch(1, routeGainMode_ ? 1 : 0);
    for (size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        row->label->setText(routeGainMode_ ? QStringLiteral("Volume") : row->property.label);
        row->label->setFixedWidth(columnWidth);
        row->valueStack->setFixedSize(valueWidth, rowHeight);
        if (row->editor) row->editor->setFixedHeight(rowHeight);
        if (row->slider) row->slider->setFixedSize(sliderWidth, sliderHeight);
        const int position = static_cast<int>(index) * (routeGainMode_ ? 2 : 1);
        const bool visible = fields_.testFlag(row->field);
        grid->setRowMinimumHeight(position, visible ? rowHeight : 0);
        if (routeGainMode_) grid->setRowMinimumHeight(position + 1, visible ? sliderHeight : 0);
    }
    const QSize contentSize(contentWidth, visibleRows * (rowHeight + (routeGainMode_ ? sliderHeight : 0)));
    if (size() != contentSize) {
        setMinimumSize(contentSize);
        setMaximumSize(contentSize);
        resize(contentSize);
        emit contentSizeChanged();
    }
}

bool NodeContentEditor::isEditing() const
{
    return std::any_of(rows_.begin(), rows_.end(), [](const auto& row) {
        return row->editingValue || (row->binding && row->binding->isEditing())
            || (row->loopHost && !row->loopHost->isHidden());
    });
}

void NodeContentEditor::finishEditing(bool committed, std::optional<NodeContentField> except)
{
    const bool wasEditing = isEditing();
    for (auto& row : rows_) {
        if (except == row->field) continue;
        if (row->editingValue) finishValueEdit(*row, committed);
        else if (row->binding && row->binding->isEditing()) {
            if (committed) row->binding->commitEdit();
            else row->binding->cancelEdit();
        }
        if (row->loopHost) {
            if (committed) row->loop->commitNumericEdit();
            else row->loop->cancelNumericEdit();
            row->loopHost->hide();
            row->valueStack->show();
        }
    }
    updateMetrics();
    if (wasEditing && !isEditing()) emit editingClosed();
}

void NodeContentEditor::finishValueEdit(Row& row, bool committed)
{
    if (!row.editingValue) return;
    if (committed) {
        row.editor->interpretText();
        row.binding->previewEdit(row.editor->value());
    }
    row.editingValue = false;
    if (committed) row.binding->commitEdit();
    else row.binding->cancelEdit();
    row.valueStack->setCurrentWidget(row.value);
    refreshValue(row);
}

bool NodeContentEditor::eventFilter(QObject* watched, QEvent* event)
{
    for (auto& row : rows_) {
        if (watched == row->value && event->type() == QEvent::MouseButtonDblClick
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            return beginValueEditingAt(row->value->mapTo(this,
                static_cast<QMouseEvent*>(event)->position().toPoint()));
        }
        if (watched == row->editor && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishValueEdit(*row, false);
            return true;
        }
        if (row->loopHost && !row->loopHost->isHidden()
            && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishEditing(false);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NodeContentEditor::mousePressEvent(QMouseEvent* event)
{
    event->setAccepted(!isDragHandleAt(event->position().toPoint()));
}

}
