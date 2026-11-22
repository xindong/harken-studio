#pragma once

#include "Widgets/Common/SourceRangeThumb.h"

namespace harken::studio {

class ActiveSourcePreview final : public SourceRangeThumb
{
    Q_OBJECT

public:
    explicit ActiveSourcePreview(const QUuid& targetGuid, QWidget* parent = nullptr);

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
