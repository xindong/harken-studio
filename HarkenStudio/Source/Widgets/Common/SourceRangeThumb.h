#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Components/VSound.h"

#include <QWidget>

#include <optional>

namespace VNX {

class SourceRangeThumb : public QWidget
{
    Q_OBJECT

public:
    explicit SourceRangeThumb(QWidget* parent = nullptr);

    void setSource(const SoundSourceDef& source, const AssetInfo* asset);
    void clearSource(const QString& text = {});

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    SoundSourceDef source_;
    std::optional<AssetInfo> asset_;
    std::shared_ptr<const WaveformPeakData> peaks_;
    QString emptyText_;
    bool sourceMissing_ = true;
};

}
