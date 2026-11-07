#include "DialogFormLayout.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QLabel>

namespace VNX {

DialogFormLayout::DialogFormLayout(QWidget* parent)
    : QGridLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setHorizontalSpacing(metrics.form.columnSpacing);
        setVerticalSpacing(metrics.form.rowSpacing);
        setColumnStretch(0, metrics.form.labelColumnShare);
        setColumnStretch(1, metrics.form.valueColumnShare);
    });
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
    QtControlSkin::setSemanticProperty(label, "textRole", QStringLiteral("formLabel"));
    auto policy = field->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    field->setSizePolicy(policy);
    addWidget(label, nextRow_, 0);
    addWidget(field, nextRow_, 1);
    ++nextRow_;
}

}
