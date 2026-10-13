#pragma once

#include "Core/BaseDialog.h"

#include <QString>

class QShowEvent;

namespace VNX {

class AboutDialog final : public BaseDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr,
                         bool showDeveloperInformation = false);

    QString diagnosticsText() const { return diagnosticsText_; }

private:
    void showEvent(QShowEvent* event) override;

    bool showDeveloperInformation_ = false;
    bool compactGeometryApplied_ = false;
    QString diagnosticsText_;
};

}
