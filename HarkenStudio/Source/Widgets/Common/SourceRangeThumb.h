#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Components/VSound.h"

#include "PreviewField.h"

#include <optional>

namespace VNX {

class SourceRangeThumb : public PreviewField
{
    Q_OBJECT

public:
    explicit SourceRangeThumb(QWidget* parent = nullptr);

    void setSource(const SoundSourceDef& source, const AssetInfo* asset);
    void clearSource(const QString& text = {});

protected:
    void paintPreview(QPainter& painter, const QRectF& bounds) override;

private:
    SoundSourceDef source_;
    std::optional<AssetInfo> asset_;
    std::shared_ptr<const WaveformPeakData> peaks_;
    QString emptyText_;
    bool sourceMissing_ = true;
};

}
