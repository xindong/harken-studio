
#include "NewProjectDialog.h"
#include "Core/ThemeLayout.h"
#include "Core/WindowPlacement.h"
#include "Widgets/Common/PanelScrollArea.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNX {

NewProjectDialog::NewProjectDialog(QWidget* parent)
    : BaseDialog(parent)
{
    setWindowTitle(tr("New Project"));
    setModal(true);
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setMinimumSize(screenBoundedWindowSize(this,
            QSize(metrics.dialog.wideWidth, metrics.dialog.wideHeight)));
    });

    auto* root = new QVBoxLayout(this);
    ThemeLayout::margins(root, ThemeLayout::Space::Panel, ThemeLayout::Space::Panel, ThemeLayout::Space::Panel, ThemeLayout::Space::Panel);
    ThemeLayout::spacing(root, ThemeLayout::Space::Section);

    chooser_ = new TemplateChooserWidget(this);
    auto* scroll = new PanelScrollArea(this);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(chooser_);
    root->addWidget(scroll, 1);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    auto* row = new QHBoxLayout();
    row->addStretch(1);
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    okButton_     = new QPushButton(tr("Create Project"), this);
    okButton_->setDefault(true);
    okButton_->setEnabled(chooser_->isValidForCreate());
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(okButton_,     &QPushButton::clicked,
            this, &NewProjectDialog::onSubmit);
    connect(chooser_, &TemplateChooserWidget::validityChanged,
            okButton_, &QPushButton::setEnabled);
    row->addWidget(cancelButton_);
    row->addWidget(okButton_);
    root->addLayout(row);
}

void NewProjectDialog::onSubmit()
{
    if (!chooser_->isValidForCreate())
        return;
    accept();
}

}
