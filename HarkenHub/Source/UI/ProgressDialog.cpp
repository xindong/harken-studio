#include "UI/ProgressDialog.h"

#include "UI/DialogLayout.h"

#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNH {

ProgressDialog::ProgressDialog(const QString& text, const QString& cancelText, QWidget* parent)
    : QDialog(parent)
{
    auto* layout = new QVBoxLayout(this);
    text_ = new DialogTextView(this);
    text_->setText(text);
    layout->addWidget(text_, 1);
    auto* progress = new QProgressBar(this);
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    layout->addWidget(progress);
    cancel_ = new QPushButton(cancelText, this);
    connect(cancel_, &QPushButton::clicked, this, &ProgressDialog::reject);
    layout->addWidget(cancel_, 0, Qt::AlignRight);
    configureDialog(this, QSize(480, 200));
}

void ProgressDialog::setLabelText(const QString& text) { text_->setText(text); }
void ProgressDialog::setCancelButtonText(const QString& text) { cancel_->setText(text); }

void ProgressDialog::reject()
{
    QDialog::reject();
    emit canceled();
}

}
