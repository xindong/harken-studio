
#pragma once

#include "TemplateChooserWidget.h"

#include "Core/BaseDialog.h"
#include <QString>

class QPushButton;

namespace VNX {

class NewProjectDialog : public BaseDialog {
    Q_OBJECT

public:
    using Result = TemplateChooserWidget::Result;

    explicit NewProjectDialog(QWidget* parent = nullptr);
    ~NewProjectDialog() override = default;

    Result result() const { return chooser_->result(); }

private slots:
    void onSubmit();

private:
    TemplateChooserWidget* chooser_     = nullptr;
    QPushButton*           okButton_    = nullptr;
    QPushButton*           cancelButton_ = nullptr;
};

}
