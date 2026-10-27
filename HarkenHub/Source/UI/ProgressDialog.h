#pragma once

#include <QDialog>

class QPushButton;

namespace VNH {

class DialogTextView;

class ProgressDialog : public QDialog
{
    Q_OBJECT

public:
    ProgressDialog(const QString& text, const QString& cancelText, QWidget* parent);
    void setLabelText(const QString& text);
    void setCancelButtonText(const QString& text);
    void reject() override;

signals:
    void canceled();

private:
    DialogTextView* text_;
    QPushButton* cancel_;
};

}
