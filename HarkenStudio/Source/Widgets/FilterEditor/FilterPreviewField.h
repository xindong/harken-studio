#pragma once

#include "Core/FilterSemantics.h"
#include "Widgets/Common/PreviewField.h"

namespace VNX {

class FilterPreviewField final : public PreviewField
{
    Q_OBJECT

public:
    explicit FilterPreviewField(QWidget* parent = nullptr);
    void setFilter(const FilterState& filter, double sampleRate = 48000.0);
    void setMixed(bool mixed);
    const FilterState& filter() const { return filter_; }

protected:
    void paintPreview(QPainter& painter, const QRectF& bounds) override;

private:
    FilterState filter_;
    double sampleRate_ = 48000.0;
    bool mixed_ = false;
};

}
