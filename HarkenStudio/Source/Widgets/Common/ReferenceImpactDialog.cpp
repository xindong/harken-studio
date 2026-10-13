#include "ReferenceImpactDialog.h"

#include "Core/BaseDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNX {

namespace {

class ReferenceImpactDialog final : public BaseDialog
{
public:
    ReferenceImpactDialog(QWidget* parent, const QString& title,
                          const QString& summary, const QString& details,
                          const QString& confirmText, bool destructive,
                          const QString& objectName)
        : BaseDialog(parent)
    {
        setObjectName(objectName);
        setWindowTitle(title);
        setMinimumSize(620, 360);

        auto* layout = new QVBoxLayout(this);
        auto* summaryLabel = new QLabel(summary, this);
        summaryLabel->setWordWrap(true);
        layout->addWidget(summaryLabel);

        auto* detailsLabel = new QLabel(tr("Affected references:"), this);
        layout->addWidget(detailsLabel);

        auto* detailsEdit = new QPlainTextEdit(this);
        detailsEdit->setObjectName(QStringLiteral("referenceImpactDetails"));
        detailsEdit->setReadOnly(true);
        detailsEdit->setPlainText(details);
        layout->addWidget(detailsEdit, 1);

        buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto* confirm = buttonBox_->addButton(
            confirmText, destructive ? QDialogButtonBox::DestructiveRole
                                     : QDialogButtonBox::AcceptRole);
        confirm->setObjectName(QStringLiteral("referenceImpactConfirmButton"));
        connect(confirm, &QPushButton::clicked, this, &QDialog::accept);
        connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttonBox_);
    }
};

}

bool confirmReferenceImpact(QWidget* parent, const QString& title,
                            const QString& summary, const QString& details,
                            const QString& confirmText, bool destructive,
                            const QString& objectName)
{
    ReferenceImpactDialog dialog(parent, title, summary, details, confirmText,
                                 destructive, objectName);
    return dialog.exec() == QDialog::Accepted;
}

}
