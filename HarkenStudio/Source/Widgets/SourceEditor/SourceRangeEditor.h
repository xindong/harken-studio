#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/Components/VSound.h"

#include <QWidget>

#include <functional>
#include <optional>

class QLabel;

namespace VNX {

class SourceEditorWaveformView;

class SourceRangeEditor final : public QWidget
{
    Q_OBJECT

public:
    SourceRangeEditor(const SoundSourceDef& source,
                     const AssetInfo* asset,
                     QWidget* parent,
                     std::function<void(const SoundSourceDef&)> apply);

    void setSource(const SoundSourceDef& source, const AssetInfo* asset);

private:
    void refreshIntegrity();
    void commit(const SoundSourceDef& source);

    std::function<void(const SoundSourceDef&)> apply_;
    SoundSourceDef source_;
    std::optional<AssetInfo> asset_;
    SourceEditorWaveformView* waveform_ = nullptr;
    QLabel* integrity_ = nullptr;
};

}
