#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Components/VSound.h"

#include <QWidget>

#include <optional>

namespace VNX {

class SourceRangeThumb final : public QWidget
{
    Q_OBJECT

public:
    explicit SourceRangeThumb(QWidget* parent = nullptr);

    void setSource(const SoundSourceDef& source, const AssetInfo* asset);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    SoundSourceDef source_;
    std::optional<AssetInfo> asset_;
    WaveformPeakData peaks_;
    bool sourceMissing_ = true;
};

}
