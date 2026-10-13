#include "SourceRangeEditor.h"

#include "SourceEditorWaveformView.h"
#include "Core/QtControlSkin.h"
#include "Core/SoundSourceSemantics.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

SourceRangeEditor::SourceRangeEditor(const SoundSourceDef& source,
                                   const AssetInfo* asset,
                                   QWidget* parent,
                                   std::function<void(const SoundSourceDef&)> apply)
    : QWidget(parent)
    , apply_(std::move(apply))
{
    setObjectName(QStringLiteral("sourceRangeEditor"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    waveform_ = new SourceEditorWaveformView(
        this, SourceEditorWaveformView::Presentation::TrimEditor);
    waveform_->setEditable(true);
    waveform_->setCommitHandler([this](const SoundSourceDef& next) { commit(next); });
    layout->addWidget(waveform_, 1);

    integrity_ = new QLabel(this);
    integrity_->setObjectName(QStringLiteral("sourceIntegrityWarning"));
    QtControlSkin::setSemanticProperty(integrity_, "textRole", QStringLiteral("warning"));
    integrity_->setContentsMargins(8, 0, 8, 4);
    integrity_->setWordWrap(true);
    layout->addWidget(integrity_);
    connect(waveform_, &SourceEditorWaveformView::previewChanged, this, [this]() {
        source_ = waveform_->previewSource();
        refreshIntegrity();
    });
    setSource(source, asset);
}

void SourceRangeEditor::setSource(const SoundSourceDef& source, const AssetInfo* asset)
{
    asset_ = asset ? std::optional<AssetInfo>{*asset} : std::nullopt;
    waveform_->setAsset(asset_ ? &*asset_ : nullptr);
    waveform_->setSource(source);
}

void SourceRangeEditor::refreshIntegrity()
{
    const QString warning = SoundSourceSemantics::integrityText(
        source_, asset_ ? &*asset_ : nullptr);
    integrity_->setText(warning);
    integrity_->setVisible(!warning.isEmpty());
}

void SourceRangeEditor::commit(const SoundSourceDef& source)
{
    source_ = source;
    refreshIntegrity();
    if (apply_)
        apply_(source_);
}

}
