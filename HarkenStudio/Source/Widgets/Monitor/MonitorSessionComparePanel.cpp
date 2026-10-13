#include "Monitor/MonitorSessionComparePanel.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorSessionComparePanel::MonitorSessionComparePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorSessionComparePanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    pickerLabel_ = new QLabel(this);
    pickerLabel_->setObjectName(QStringLiteral("MonitorSessionComparePicker"));
    pickerLabel_->setWordWrap(true);
    root->addWidget(pickerLabel_);
    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(QStringLiteral("MonitorSessionCompareResult"));
    root->addWidget(resultLabel_);
    exportLabel_ = new QLabel(this);
    exportLabel_->setObjectName(QStringLiteral("MonitorSessionCompareExport"));
    exportLabel_->setWordWrap(true);
    root->addWidget(exportLabel_);
    applyState({});
}

void MonitorSessionComparePanel::applyState(const Monitor::MonitorCompareUiState& state)
{
    if (pickerLabel_)
    {
        if (!state.pickerWired)
            pickerLabel_->setText(QStringLiteral("File picker not wired"));
        else if (state.picker == Monitor::MonitorComparePicker::MissingSecondFile)
            pickerLabel_->setText(QStringLiteral("Second file missing"));
        else
            pickerLabel_->setText(QStringLiteral("Loaded"));
    }
    if (resultLabel_)
        resultLabel_->setText(QString::fromLatin1(
            Monitor::compareResultText(state.compareResult)));
    if (exportLabel_)
        exportLabel_->setText(QString::fromStdString(
            Monitor::compareExportText(state.summary)));
}

QString MonitorSessionComparePanel::resultText() const
{
    return resultLabel_ ? resultLabel_->text() : QString();
}

QString MonitorSessionComparePanel::pickerText() const
{
    return pickerLabel_ ? pickerLabel_->text() : QString();
}

QString MonitorSessionComparePanel::exportText() const
{
    return exportLabel_ ? exportLabel_->text() : QString();
}

} // namespace VNX
