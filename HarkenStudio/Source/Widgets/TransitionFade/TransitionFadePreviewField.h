#pragma once

#include "TransitionRuleEditing.h"
#include "Widgets/Common/PreviewField.h"

namespace VNX {

class TransitionFadePreviewField final : public PreviewField
{
    Q_OBJECT

public:
    explicit TransitionFadePreviewField(QWidget* parent = nullptr);
    void setPreview(const TransitionPreview& preview, bool active);

protected:
    void paintPreview(QPainter& painter, const QRectF& bounds) override;

private:
    TransitionPreview preview_;
    bool active_ = false;
};

}
