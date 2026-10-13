#include "ParameterValueEditor.h"

#include "DecimalSpinBox.h"
#include "ParameterBinding.h"
#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/QtControlSkin.h"
#include "Core/ReentrancyGuard.h"
#include "Core/ThemeManager.h"
#include "Core/VariableSemantics.h"

#include <QAbstractListModel>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QListView>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

namespace VNX {

class DiscreteValueModel : public QAbstractListModel
{
public:
    explicit DiscreteValueModel(ParameterBinding* binding, QObject* parent)
        : QAbstractListModel(parent), binding_(binding) {}

    void refresh()
    {
        beginResetModel();
        if (!binding_) {
            first_ = 0;
            count_ = 0;
            endResetModel();
            return;
        }
        const double first = std::max(std::ceil(binding_->minimum()),
            -static_cast<double>(VariableSemantics::kStepExactIntBound));
        const double last = std::min(std::floor(binding_->maximum()),
            static_cast<double>(VariableSemantics::kStepExactIntBound));
        first_ = first <= last ? static_cast<int>(first) : 0;
        count_ = binding_->discreteSelection() && first <= last
            ? static_cast<int>(last - first + 1) : 0;
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : count_;
    }

    int rowForValue(double value) const
    {
        const double row = value - first_;
        return row >= 0 && row < count_ && row == std::floor(row) ? static_cast<int>(row) : -1;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!binding_ || !index.isValid() || index.row() < 0 || index.row() >= count_) return {};
        const int value = first_ + index.row();
        if (role == Qt::UserRole) return value;
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole || role == Qt::ToolTipRole) {
            const QString label = binding_->annotationFor(value);
            if (label.isEmpty()) return QString::number(value);
            return role == Qt::ToolTipRole
                ? QStringLiteral("%1 (%2)").arg(label).arg(value) : label;
        }
        return {};
    }

    QModelIndexList match(const QModelIndex& start, int role, const QVariant& value,
                         int hits = 1, Qt::MatchFlags flags = Qt::MatchStartsWith | Qt::MatchWrap) const override
    {
        if (role == Qt::UserRole
            && (flags == Qt::MatchExactly || flags == (Qt::MatchExactly | Qt::MatchCaseSensitive))) {
            const int row = rowForValue(value.toDouble());
            return row >= 0 ? QModelIndexList{index(row, 0)} : QModelIndexList{};
        }
        return QAbstractListModel::match(start, role, value, hits, flags);
    }

private:
    QPointer<ParameterBinding> binding_;
    int first_ = 0;
    int count_ = 0;
};

ParameterValueEditor::ParameterValueEditor(ParameterBinding* binding, ThemeManager* themeManager,
                                         QWidget* parent)
    : QWidget(parent)
    , binding_(binding)
    , themeManager_(themeManager)
    , spinBox_(new DecimalSpinBox(this))
    , comboBox_(new QComboBox(this))
    , choiceModel_(new DiscreteValueModel(binding_, comboBox_))
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    spinBox_->setMinimumWidth(0);
    spinBox_->setSizePolicy(QSizePolicy::Ignored, spinBox_->sizePolicy().verticalPolicy());
    spinBox_->setKeyboardTracking(false);
    row->addWidget(spinBox_, 1);
    comboBox_->setObjectName(QStringLiteral("parameterValueChoice"));
    comboBox_->setMinimumWidth(0);
    comboBox_->setSizePolicy(QSizePolicy::Ignored, comboBox_->sizePolicy().verticalPolicy());
    comboBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comboBox_->setMinimumContentsLength(5);
    comboBox_->setMaxVisibleItems(16);
    auto* view = new QListView(comboBox_);
    view->setUniformItemSizes(true);
    comboBox_->setView(view);
    comboBox_->setModel(choiceModel_);
    row->addWidget(comboBox_, 1);

    connect(spinBox_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (binding_ && !updating_)
            binding_->commitValue(binding_->fromDisplayValue(value));
    });
    connect(comboBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (binding_ && !updating_ && index >= 0)
            binding_->commitValue(comboBox_->itemData(index).toDouble());
    });
    connect(binding_, &ParameterBinding::metadataChanged, this, &ParameterValueEditor::syncMetadata);
    connect(binding_, &ParameterBinding::valueUpdated, this, &ParameterValueEditor::syncValue);
    connect(binding_, &QObject::destroyed, this, [this] {
        setEnabled(false);
        choiceModel_->refresh();
    });
    connect(themeManager_, &ThemeManager::themeChanged, this, [this] { setCompact(compact_); });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] { setCompact(compact_); });
    setCompact(false);
    syncMetadata();
}

QDoubleSpinBox* ParameterValueEditor::valueEditor() const
{
    return binding_ && !binding_->discreteSelection() ? spinBox_ : nullptr;
}

QComboBox* ParameterValueEditor::choiceEditor() const
{
    return binding_ && binding_->discreteSelection() ? comboBox_ : nullptr;
}

QSize ParameterValueEditor::sizeHint() const
{
    return binding_ && binding_->discreteSelection() ? comboBox_->sizeHint() : spinBox_->sizeHint();
}

void ParameterValueEditor::setCompact(bool compact)
{
    compact_ = compact;
    spinBox_->setButtonSymbols(compact ? QAbstractSpinBox::NoButtons : QAbstractSpinBox::UpDownArrows);
    spinBox_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spinBox_->setFont(compact
        ? FontManager::instance().getFont(themeManager_->snapshot(), FontRole::Mono) : font());
    const QString role = compact ? QStringLiteral("inspectorValue") : QStringLiteral("compact");
    QtControlSkin::setSemanticProperty(spinBox_, "fieldRole", role);
    QtControlSkin::setSemanticProperty(comboBox_, "fieldRole", role);
}

void ParameterValueEditor::cancelPendingInput()
{
    if (!binding_)
        return;
    const QSignalBlocker spinBlocker(spinBox_);
    const QSignalBlocker choiceBlocker(comboBox_);
    spinBox_->setValue(binding_->toDisplayValue(binding_->value()));
    comboBox_->setCurrentIndex(binding_->mixedValue() ? -1
        : choiceModel_->rowForValue(binding_->value()));
}

bool ParameterValueEditor::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange)
        syncToolTips();
    return QWidget::event(event);
}

void ParameterValueEditor::syncMetadata()
{
    if (!binding_)
        return;
    ReentrancyGuard guard(updating_);
    const QString name = binding_->name();
    setAccessibleName(name);
    spinBox_->setDecimals(binding_->decimals());
    spinBox_->setRange(binding_->toDisplayValue(binding_->minimum()),
                      binding_->toDisplayValue(binding_->maximum()));
    spinBox_->setAccessibleName(name);
    spinBox_->setValue(binding_->toDisplayValue(binding_->value()));
    spinBox_->setVisible(!binding_->discreteSelection());
    comboBox_->setVisible(binding_->discreteSelection());
    comboBox_->setAccessibleName(name);
    choiceModel_->refresh();
    comboBox_->setEnabled(choiceModel_->rowCount() > 0);
    comboBox_->setCurrentIndex(binding_->mixedValue() ? -1
        : choiceModel_->rowForValue(binding_->value()));
    setFocusProxy(binding_->discreteSelection()
        ? static_cast<QWidget*>(comboBox_) : static_cast<QWidget*>(spinBox_));
    syncToolTips();
}

void ParameterValueEditor::syncValue(double value)
{
    if (!binding_ || updating_)
        return;
    ReentrancyGuard guard(updating_);
    spinBox_->setValue(binding_->toDisplayValue(value));
    comboBox_->setCurrentIndex(choiceModel_->rowForValue(value));
    syncToolTips();
}

void ParameterValueEditor::syncToolTips()
{
    if (!binding_)
        return;
    QString text = toolTip();
    if (!binding_->mixedValue()) {
        if (!text.isEmpty()) text += QLatin1Char('\n');
        text += QStringLiteral("%1 %2").arg(
            formatDecimal(binding_->toDisplayValue(binding_->value()), binding_->decimals()),
            binding_->unit().trimmed()).trimmed();
    }
    spinBox_->setToolTip(text);
    comboBox_->setToolTip(text);
}

}
