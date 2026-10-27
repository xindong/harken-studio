#include "UI/MessageDialog.h"

#include "UI/DialogLayout.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextDocument>
#include <QVBoxLayout>

namespace VNH {

MessageDialog::MessageDialog(QMessageBox::Icon icon, const QString& title,
                             const QString& text, QMessageBox::StandardButtons buttons,
                             QWidget* parent)
    : QDialog(parent), message_(text)
{
    setWindowTitle(title);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);
    auto* body = new QHBoxLayout();
    if (icon != QMessageBox::NoIcon) {
        const auto pixmap = icon == QMessageBox::Warning ? QStyle::SP_MessageBoxWarning
            : icon == QMessageBox::Critical ? QStyle::SP_MessageBoxCritical
            : icon == QMessageBox::Question ? QStyle::SP_MessageBoxQuestion
            : QStyle::SP_MessageBoxInformation;
        auto* image = new QLabel(this);
        image->setPixmap(style()->standardIcon(pixmap).pixmap(32, 32));
        body->addWidget(image, 0, Qt::AlignTop);
    }
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("messageText"));
    text_->setReadOnly(true);
    text_->setPlainText(text);
    body->addWidget(text_, 1);
    layout->addLayout(body, 1);
    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::StandardButtons(static_cast<int>(buttons)), this);
    connect(buttons_, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        clicked_ = button;
        done(static_cast<int>(buttons_->standardButton(button)));
    });
    layout->addWidget(buttons_);

    QTextDocument measurement;
    measurement.setDefaultFont(text_->font());
    measurement.setPlainText(text);
    measurement.setTextWidth(460);
    const int height = qBound(180, qCeil(measurement.size().height()) + 100, 400);
    configureDialog(this, QSize(560, height));
}

QMessageBox::StandardButton MessageDialog::information(QWidget* parent, const QString& title,
                                                       const QString& text)
{
    MessageDialog dialog(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    return static_cast<QMessageBox::StandardButton>(dialog.exec());
}

QMessageBox::StandardButton MessageDialog::warning(QWidget* parent, const QString& title,
                                                   const QString& text)
{
    MessageDialog dialog(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    return static_cast<QMessageBox::StandardButton>(dialog.exec());
}

QMessageBox::StandardButton MessageDialog::critical(QWidget* parent, const QString& title,
                                                    const QString& text)
{
    MessageDialog dialog(QMessageBox::Critical, title, text, QMessageBox::Ok, parent);
    return static_cast<QMessageBox::StandardButton>(dialog.exec());
}

QMessageBox::StandardButton MessageDialog::question(QWidget* parent, const QString& title,
                                                    const QString& text,
                                                    QMessageBox::StandardButtons buttons,
                                                    QMessageBox::StandardButton defaultButton)
{
    MessageDialog dialog(QMessageBox::Question, title, text, buttons, parent);
    dialog.setDefaultButton(qobject_cast<QPushButton*>(dialog.button(defaultButton)));
    return static_cast<QMessageBox::StandardButton>(dialog.exec());
}

QPushButton* MessageDialog::addButton(const QString& text, QMessageBox::ButtonRole role)
{
    return buttons_->addButton(text, static_cast<QDialogButtonBox::ButtonRole>(role));
}

QAbstractButton* MessageDialog::clickedButton() const { return clicked_; }
QAbstractButton* MessageDialog::button(QMessageBox::StandardButton button) const
{
    return buttons_->button(static_cast<QDialogButtonBox::StandardButton>(button));
}
QList<QAbstractButton*> MessageDialog::buttons() const { return buttons_->buttons(); }
QMessageBox::ButtonRole MessageDialog::buttonRole(QAbstractButton* button) const
{
    return static_cast<QMessageBox::ButtonRole>(buttons_->buttonRole(button));
}
QMessageBox::StandardButtons MessageDialog::standardButtons() const
{
    return QMessageBox::StandardButtons(static_cast<int>(buttons_->standardButtons()));
}
void MessageDialog::setDefaultButton(QPushButton* button) { button->setDefault(true); }
void MessageDialog::setInformativeText(const QString& text)
{
    text_->setPlainText(message_ + QStringLiteral("\n\n") + text);
}
QString MessageDialog::text() const { return message_; }

void MessageDialog::reject()
{
    if (auto* cancel = button(QMessageBox::Cancel)) {
        cancel->click();
        return;
    }
    for (auto* button : buttons()) {
        if (buttonRole(button) == QMessageBox::RejectRole
            || buttonRole(button) == QMessageBox::NoRole) {
            button->click();
            return;
        }
    }
    QDialog::reject();
}

}
