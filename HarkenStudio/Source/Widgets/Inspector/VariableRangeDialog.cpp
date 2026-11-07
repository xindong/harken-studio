#include "Core/ThemeManager.h"
#include "VariableRangeDialog.h"

#include "Core/BaseDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNX {

std::optional<QString> chooseVariableRangePolicy(QWidget* parent, const QJsonObject& change)
{
    BaseDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("variableRangePolicyDialog"));
    dialog.setWindowTitle(QObject::tr("Change Variable Range"));
    ThemeManager::instance().bindMetrics(&dialog, [&dialog](const ThemeMetrics& metrics) {
        dialog.setMinimumWidth(metrics.dialog.preferredWidth);
    });
    auto* layout = new QVBoxLayout(&dialog);
    const auto label = [&](const QString& text) {
        auto* widget = new QLabel(text, &dialog);
        widget->setTextFormat(Qt::PlainText);
        widget->setWordWrap(true);
        layout->addWidget(widget);
    };
    const double oldMin = change.value(QStringLiteral("old_range_min")).toDouble();
    const double oldMax = change.value(QStringLiteral("old_range_max")).toDouble();
    label(QObject::tr("'%1': [%2, %3] \u2192 [%4, %5]")
        .arg(change.value(QStringLiteral("name")).toString()).arg(oldMin).arg(oldMax)
        .arg(change.value(QStringLiteral("range_min")).toDouble())
        .arg(change.value(QStringLiteral("range_max")).toDouble()));
    label(QObject::tr("Stretch: Scale curves and values proportionally to the new range."));
    label(QObject::tr("Fix: Keep curve X coordinates. Trim curves and clamp values to the new range."));
    const bool canStretch = oldMin < oldMax;
    if (!canStretch)
        label(QObject::tr("The current range has zero width. Use Fix."));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* stretch = buttons->addButton(QObject::tr("Stretch"), QDialogButtonBox::AcceptRole);
    auto* fix = buttons->addButton(QObject::tr("Fix"), QDialogButtonBox::ActionRole);
    stretch->setObjectName(QStringLiteral("variableRangeStretch"));
    fix->setObjectName(QStringLiteral("variableRangeFix"));
    stretch->setEnabled(canStretch);
    stretch->setAutoDefault(false);
    fix->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    QString policy;
    QObject::connect(stretch, &QPushButton::clicked, &dialog, [&] {
        policy = QStringLiteral("stretch");
        dialog.accept();
    });
    QObject::connect(fix, &QPushButton::clicked, &dialog, [&] {
        policy = QStringLiteral("fix");
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return policy;
}

}
