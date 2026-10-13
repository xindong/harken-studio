
#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <initializer_list>

namespace VNX {

class BaseDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BaseDialog(QWidget* parent = nullptr,
                        Qt::WindowFlags flags = Qt::WindowFlags());
    ~BaseDialog() override = default;

protected:

    virtual void applyTheme();


    void setupStandardButtons(QDialogButtonBox::StandardButtons buttons);

    QDialogButtonBox* buttonBox() const { return buttonBox_; }


    void setDialogTabOrder(std::initializer_list<QWidget*> widgets);


    void showEvent(QShowEvent* event) override;

protected:
    QDialogButtonBox* buttonBox_ = nullptr;
    bool autoFramelessChrome_ = true;

private:
    bool chromeApplied_ = false;
};

}
