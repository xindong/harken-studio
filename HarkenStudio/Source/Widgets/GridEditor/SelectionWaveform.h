#pragma once

#include "Widgets/Common/SourceRangeThumb.h"

namespace VNX {

class SelectionWaveform final : public SourceRangeThumb
{
    Q_OBJECT

public:
    explicit SelectionWaveform(const QUuid& targetGuid, QWidget* parent = nullptr);

signals:
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

private:
    void refresh();
    void clear();

    QUuid targetGuid_;
    QUuid entryId_;
    uint32_t sourceId_ = 0;
};

}
