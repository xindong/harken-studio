#pragma once

#include <QDialog>
#include <QMessageBox>

class QDialogButtonBox;
class QPlainTextEdit;

namespace VNH {

class MessageDialog : public QDialog
{
    Q_OBJECT

public:
    MessageDialog(QMessageBox::Icon icon, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QWidget* parent);

    static QMessageBox::StandardButton information(QWidget* parent, const QString& title,
                                                   const QString& text);
    static QMessageBox::StandardButton warning(QWidget* parent, const QString& title,
                                               const QString& text);
    static QMessageBox::StandardButton critical(QWidget* parent, const QString& title,
                                                const QString& text);
    static QMessageBox::StandardButton question(QWidget* parent, const QString& title,
                                                const QString& text,
                                                QMessageBox::StandardButtons buttons,
                                                QMessageBox::StandardButton defaultButton);

    QPushButton* addButton(const QString& text, QMessageBox::ButtonRole role);
    QAbstractButton* clickedButton() const;
    QAbstractButton* button(QMessageBox::StandardButton button) const;
    QList<QAbstractButton*> buttons() const;
    QMessageBox::ButtonRole buttonRole(QAbstractButton* button) const;
    QMessageBox::StandardButtons standardButtons() const;
    void setDefaultButton(QPushButton* button);
    void setInformativeText(const QString& text);
    QString text() const;
    void reject() override;

private:
    QPlainTextEdit* text_;
    QDialogButtonBox* buttons_;
    QAbstractButton* clicked_ = nullptr;
    QString message_;
};

}
