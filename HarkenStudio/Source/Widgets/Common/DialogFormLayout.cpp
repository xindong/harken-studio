#include "DialogFormLayout.h"

#include "Core/QtControlSkin.h"

#include <QLabel>

namespace VNX {

DialogFormLayout::DialogFormLayout(QWidget* parent)
    : QGridLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    setHorizontalSpacing(16);
    setVerticalSpacing(RowSpacing);
    setColumnStretch(0, 3);
    setColumnStretch(1, 7);
}

void DialogFormLayout::addRow(const QString& text, QWidget* field)
{
    addRow(new QLabel(text), field);
}

void DialogFormLayout::addRow(QLabel* label, QWidget* field)
{
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setWordWrap(true);
    label->setBuddy(field);
    QtControlSkin::setSemanticProperty(label, "dialogRole", QStringLiteral("label"));
    auto policy = field->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    field->setSizePolicy(policy);
    addWidget(label, nextRow_, 0);
    addWidget(field, nextRow_, 1);
    ++nextRow_;
}

}
