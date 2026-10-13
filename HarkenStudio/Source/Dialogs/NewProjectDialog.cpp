
#include "NewProjectDialog.h"

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
    setMinimumSize(1100, 600);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(14);

    chooser_ = new TemplateChooserWidget(this);
    root->addWidget(chooser_, 1);

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
