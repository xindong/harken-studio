#pragma once

#include "MeterRenderCommon.h"
#include "MeterBallistics.h"
#include "MeterSource.h"
#include "MeterScale.h"
#include "SpectrogramHistory.h"
#include "Widgets/Common/AxisCrosshair.h"

#include <QElapsedTimer>
#include <QWidget>
#include <QVariantMap>
#include <harken_studio/ObjectId.h>
#include <optional>

class QTimer;
class QComboBox;
class QLabel;
class QPainter;
class QToolButton;

namespace VNX {

class ObjectRefWidget;
class ParameterSlider;

class MeterPanel : public QWidget
{
    Q_OBJECT

public:
    enum class Mode { Peak = 0, Spectrum = 1, Spectrogram = 2, Loudness = 3 };
    Q_ENUM(Mode)
    enum class Presentation { Standalone, Embedded };
    enum class LoudnessMeasurement { Rms, Lufs };
    Q_ENUM(LoudnessMeasurement)

    explicit MeterPanel(QWidget* parent = nullptr, Presentation presentation = Presentation::Standalone);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    float levelDb(int channel = 0) const { return displayState().peakBallistics[channel].levelDb; }
    float heldPeakDb(int channel = 0) const { return displayState().peakBallistics[channel].peakDb; }
    bool clipped(int channel = 0) const { return displayState().clipped[channel]; }
    bool levelAvailable() const { return displayState().levelAvailable; }
    uint32_t busObjectID() const { return busObjectID_; }
    Mode mode() const { return mode_; }
    LoudnessMeasurement loudnessMeasurement() const { return loudnessMeasurement_; }
    float loudnessDb() const { return displayState().loudnessBallistics.levelDb; }
    float heldLoudnessDb() const { return displayState().loudnessBallistics.peakDb; }
    QRect plotBounds() const;
    AxisCrosshair crosshair() const;
    double amplitudeExponent() const;
    bool measurementAvailable() const { return displayState().measurementAvailable; }
    int historyColumns() const { return displayState().spectrogram.columns(); }
    const auto& spectrumLevels() const { return displayState().spectrumLevels; }
    const auto& spectrumPeaks() const { return displayState().spectrumPeaks; }
    double spectrogramSpeed() const { return spectrogramSpeed_; }
    void acceptMeasurement(const HarkenEditor::Bridge::MeterMeasurementFrame& frame);
    QVariantMap saveState() const;
    void restoreState(const QVariantMap& state);
    int embeddedHeaderHeight() const { return header_->height(); }
    void setEmbeddedPreferredWidth(int width);

public slots:
    void setBus(uint32_t objectID);
    void setMode(Mode mode);
    void setLoudnessMeasurement(LoudnessMeasurement measurement);
    void acceptPeak(float left, float right);
    void resetLevels();
    void resetPeakHold();
    void setAmplitudeExponent(double exponent);
    void setSpectrogramSpeed(double speed);

signals:
    void detachRequested(const QVariantMap& state);
    void expandedChanged(bool expanded);
    void embeddedHeaderHeightChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void sampleLevels();
    void refresh();

private:
    struct DisplayState
    {
        std::array<MeterBallistics, 2> peakBallistics;
        std::array<bool, 2> clipped{};
        bool levelAvailable = false;
        bool measurementAvailable = false;
        MeterBallistics loudnessBallistics;
        uint32_t spectrumSampleRate = 48000;
        std::array<float, kSpectrumMeterDisplayPoints> spectrumLevels{};
        std::array<float, kSpectrumMeterDisplayPoints> spectrumPeaks{};
        SpectrogramHistory spectrogram;
    };

    const DisplayState& displayState() const { return pausedDisplay_ ? *pausedDisplay_ : liveDisplay_; }
    void setDisplayPaused(bool paused);
    void resetMeasurements();
    void sampleMeasurements();
    void paintCanvas(QPainter& painter);
    void paintMeasurement(QPainter& painter, const QRect& bounds);
    void updateDisplay();
    void layoutSpectrogramSpeedControl();
    void advanceBallistics();
    void updateSpectrumLevels();
    AmplitudeScale amplitudeAxis() const;
    QRect canvasBounds() const;
    PeakMeterLayout peakLayout() const;
    PeakMeterLayoutMode peakLayoutMode() const;
    int fullPeakWidth() const;
    void updatePresentation();
    bool isLevelMode() const { return mode_ == Mode::Peak || mode_ == Mode::Loudness; }

    Presentation presentation_;
    int expandedWidth_ = 0;
    QWidget* header_ = nullptr;
    QToolButton* expandButton_ = nullptr;
    QToolButton* detachButton_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    ObjectRefWidget* busSelector_ = nullptr;
    QComboBox* modeSelector_ = nullptr;
    QLabel* masterLabel_ = nullptr;
    QWidget* canvas_ = nullptr;
    ParameterSlider* spectrogramSpeedControl_ = nullptr;
    std::optional<QPointF> hoverPosition_;
    Mode mode_ = Mode::Peak;
    LoudnessMeasurement loudnessMeasurement_ = LoudnessMeasurement::Rms;
    double peakAmplitudeExponent_ = 0.3;
    double spectrumAmplitudeExponent_ = 0.3;
    MeterSource source_;
    QElapsedTimer retryTimer_;
    DisplayState liveDisplay_;
    std::optional<DisplayState> pausedDisplay_;
    std::array<MeterBallistics, kSpectrumMeterDisplayPoints> spectrumBallistics_{};
    double spectrogramSpeed_ = 2.0;
    uint32_t busObjectID_ = 0;
    harkenstudio::ObjectId busRuntimeId_{};
    QElapsedTimer elapsed_;
};

}
