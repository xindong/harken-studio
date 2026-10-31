#include "MultiSelectComboBox.h"

#include "Core/QtControlSkin.h"

#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QStandardItemModel>
#include <QStylePainter>
#include <QWheelEvent>

namespace VNX {

MultiSelectComboBox::MultiSelectComboBox(QWidget* parent)
    : QComboBox(parent)
    , items_(static_cast<QStandardItemModel*>(model()))
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setPlaceholderText(tr("None"));
    setView(new QListView(this));
    setProperty("popupHasCheckableItems", true);
    QtControlSkin::applyPopupStyle(this);
    view()->installEventFilter(this);
    view()->viewport()->installEventFilter(this);
    connect(items_, &QStandardItemModel::dataChanged,
            this, &MultiSelectComboBox::updateSelection);
    connect(items_, &QStandardItemModel::rowsInserted,
            this, &MultiSelectComboBox::updateSelection);
    connect(items_, &QStandardItemModel::rowsRemoved,
            this, &MultiSelectComboBox::updateSelection);
    connect(items_, &QStandardItemModel::modelReset,
            this, &MultiSelectComboBox::updateSelection);
}

void MultiSelectComboBox::setOptions(const QList<QPair<QString, QVariant>>& options)
{
    bool unchanged = options.size() == count();
    for (int i = 0; unchanged && i < count(); ++i)
        unchanged = itemText(i) == options[i].first && itemData(i) == options[i].second;
    if (unchanged) return;

    {
        const QScopedValueRollback guard(selectionUpdating_, true);
        items_->clear();
        for (const auto& [text, value] : options) {
            auto* item = new QStandardItem(text);
            item->setData(value, Qt::UserRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(selectedData_.contains(value) ? Qt::Checked : Qt::Unchecked);
            items_->appendRow(item);
        }
    }
    updateSelection();
}

void MultiSelectComboBox::setSelectedData(const QVariantList& values)
{
    {
        const QScopedValueRollback guard(selectionUpdating_, true);
        for (int i = 0; i < count(); ++i)
            items_->item(i)->setCheckState(values.contains(itemData(i)) ? Qt::Checked : Qt::Unchecked);
    }
    updateSelection();
}

QSize MultiSelectComboBox::minimumSizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    return style()->sizeFromContents(QStyle::CT_ComboBox, &option,
        QSize(0, fontMetrics().height()), this);
}

void MultiSelectComboBox::showPopup()
{
    pressedIndex_ = QPersistentModelIndex();
    QComboBox::showPopup();
}

bool MultiSelectComboBox::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == view()->viewport()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonRelease)) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const auto index = view()->indexAt(mouse->position().toPoint());
            if (event->type() == QEvent::MouseButtonRelease) {
                const bool toggle = index.isValid() && index == pressedIndex_;
                pressedIndex_ = QPersistentModelIndex();
                if (toggle) toggleItem(index);
            } else {
                pressedIndex_ = index;
                if (index.isValid()) view()->setCurrentIndex(index);
            }
            return true;
        }
    }
    if (watched == view() && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space) {
            if (!key->isAutoRepeat()) toggleItem(view()->currentIndex());
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            hidePopup();
            return true;
        }
    }
    return QComboBox::eventFilter(watched, event);
}

void MultiSelectComboBox::paintEvent(QPaintEvent*)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.currentIcon = QIcon();
    option.currentText = selectedText_.isEmpty() ? placeholderText() : selectedText_;
    const auto textColor = option.palette.color(selectedText_.isEmpty()
        ? QPalette::PlaceholderText : QPalette::ButtonText);
    option.palette.setColor(QPalette::ButtonText, textColor);
    option.palette.setColor(QPalette::Text, textColor);
    const QRect textRect = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                  QStyle::SC_ComboBoxEditField, this);
    option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight, textRect.width());
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

void MultiSelectComboBox::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Up:
    case Qt::Key_Down:
        showPopup();
        event->accept();
        return;
    default:
        QComboBox::keyPressEvent(event);
    }
}

void MultiSelectComboBox::wheelEvent(QWheelEvent* event)
{
    event->ignore();
}

void MultiSelectComboBox::toggleItem(const QModelIndex& index)
{
    if (!index.isValid() || !index.flags().testFlag(Qt::ItemIsEnabled)
        || !index.flags().testFlag(Qt::ItemIsUserCheckable)) return;
    items_->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                              ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
}

void MultiSelectComboBox::updateSelection()
{
    if (selectionUpdating_) return;
    QVariantList values;
    QStringList labels;
    for (int i = 0; i < count(); ++i) {
        if (itemData(i, Qt::CheckStateRole).toInt() == Qt::Checked) {
            values.append(itemData(i));
            labels.append(itemText(i));
        }
    }
    selectedText_ = labels.join(QStringLiteral(", "));
    setToolTip(selectedText_);
    setAccessibleDescription(selectedText_);
    updateGeometry();
    update();
    if (selectedData_ == values) return;
    selectedData_ = values;
    emit selectedDataChanged(values);
}

}
