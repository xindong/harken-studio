#include "MeterPanel.h"

#include "Core/AudioEngine.h"
#include "Core/FontManager.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "PeakMeterRenderer.h"
#include "FrequencyScale.h"
#include "MeterColorMap.h"
#include "SpectrumMeterRenderer.h"
#include "SpectrogramMeterRenderer.h"
#include "Widgets/Common/AmplitudeAxis.h"
#include "Widgets/Common/AmplitudeCurvePopup.h"
#include "Widgets/Common/ElidedLabel.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QHideEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {
namespace {

constexpr double kSpectrogramHistorySeconds = 40.0;
constexpr double kSpectrogramPixelsPerSecond = 40.0;
constexpr double kSpectrogramMinSpeed = 0.25;
constexpr double kSpectrogramMaxSpeed = 10.0;

QFont peakValueFont()
{
    return FontManager::instance().getFont(FontRole::Mono,
        ThemeManager::instance().snapshot()->ui.typography.label.pointSize + 2, QFont::Bold);
}

}

MeterPanel::MeterPanel(QWidget* parent, Presentation presentation)
    : QWidget(parent)
    , presentation_(presentation)
    , refreshTimer_(new QTimer(this))
{
    elapsed_.start();
    setObjectName(QStringLiteral("MeterPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);
    busSelector_ = new ObjectRefWidget(this);
    busSelector_->setObjectName(QStringLiteral("meterBusSelector"));
    busSelector_->setFilterType(ProjectItem::Type::Bus);
    busSelector_->setSelectorTitle(tr("Meter Bus"));
    masterLabel_ = new ElidedLabel(tr("Master"), this);
    masterLabel_->setAlignment(Qt::AlignCenter);
    modeSelector_ = new QComboBox(this);
    modeSelector_->setObjectName(QStringLiteral("meterModeSelector"));
    modeSelector_->addItem(tr("Peak"), static_cast<int>(Mode::Peak));
    modeSelector_->addItem(tr("Loudness"), static_cast<int>(Mode::Loudness));
    modeSelector_->addItem(tr("Spectrum"), static_cast<int>(Mode::Spectrum));
    modeSelector_->addItem(tr("Spectrogram"), static_cast<int>(Mode::Spectrogram));
    masterLabel_->setFixedHeight(modeSelector_->sizeHint().height());
    if (presentation_ == Presentation::Embedded) {
        root->setContentsMargins(0, 0, 1, 0);
        root->setSpacing(0);
        header_ = new QWidget(this);
        header_->setContentsMargins(0, 0, 0, 1);
        header_->installEventFilter(this);
        auto* header = new QHBoxLayout(header_);
        header->setContentsMargins(4, 2, 4, 2);
        header->setSpacing(4);
        expandButton_ = new QToolButton(this);
        expandButton_->setObjectName(QStringLiteral("meterExpandButton"));
        expandButton_->setCheckable(true);
        detachButton_ = new QToolButton(this);
        detachButton_->setObjectName(QStringLiteral("meterDetachButton"));
        detachButton_->setToolTip(tr("Open a new meter"));
        detachButton_->setAccessibleName(detachButton_->toolTip());
        for (auto* button : {expandButton_, detachButton_}) {
            button->setAutoRaise(true);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setProperty("flatIconButton", true);
        }
        header->addWidget(expandButton_, 0, Qt::AlignLeft);
        header->addWidget(busSelector_, 1);
        header->addWidget(masterLabel_, 1);
        header->addWidget(detachButton_);
        root->addWidget(header_);
        connect(expandButton_, &QToolButton::toggled, this, [this]() {
            updatePresentation();
            resize(sizeHint().width(), height());
            emit expandedChanged(expandButton_->isChecked());
        });
        connect(detachButton_, &QToolButton::clicked, this,
            [this] { emit detachRequested(saveState()); });
    } else {
        root->addWidget(busSelector_);
        root->addWidget(masterLabel_);
        root->addWidget(modeSelector_);
    }
    connect(modeSelector_, &QComboBox::currentIndexChanged, this,
        [this](int index) { setMode(static_cast<Mode>(modeSelector_->itemData(index).toInt())); });
    canvas_ = new QWidget(this);
    canvas_->setObjectName(QStringLiteral("meterCanvas"));
    canvas_->setMouseTracking(true);
    canvas_->installEventFilter(this);
    root->addWidget(canvas_, 1);
    spectrogramSpeedControl_ = new ParameterSlider(tr("Scroll speed"), canvas_);
    spectrogramSpeedControl_->setObjectName(QStringLiteral("meterSpectrogramSpeed"));
    spectrogramSpeedControl_->setProperty("sliderStyle", QStringLiteral("compact"));
    spectrogramSpeedControl_->setPresenterOverride(ParameterPresenterId::HorizontalSlider);
    spectrogramSpeedControl_->setRange(kSpectrogramMinSpeed, kSpectrogramMaxSpeed, 2);
    spectrogramSpeedControl_->setSuffix(QStringLiteral("x"));
    spectrogramSpeedControl_->binding()->setQuantum(0.01);
    spectrogramSpeedControl_->binding()->setDefaultValue(spectrogramSpeed_);
    spectrogramSpeedControl_->setValue(spectrogramSpeed_);
    spectrogramSpeedControl_->hide();
    connect(spectrogramSpeedControl_->binding(), &ParameterBinding::valueUpdated,
        this, &MeterPanel::setSpectrogramSpeed);
    connect(busSelector_, &ObjectRefWidget::objectChanged, this, &MeterPanel::setBus);
    auto& projects = ProjectManager::instance();
    connect(&projects, &ProjectManager::projectOpened, this, [this] { setBus(0); });
    connect(&projects, &ProjectManager::projectClosed, this, [this] { setBus(0); });
    connect(&projects, &ProjectManager::itemDeleted, this, [this] {
        if (busObjectID_ != 0 && !ProjectManager::instance().findBusItem(busObjectID_))
            setBus(0);
    });
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &MeterPanel::refresh);
    auto& engine = AudioEngine::instance();
    connect(&engine, &AudioEngine::meterLevelsUpdated, this, &MeterPanel::sampleLevels);
    connect(&engine, &AudioEngine::playingChanged, this, [this](bool playing) {
        if (!playing) {
            source_.stop();
            resetLevels();
        } else {
            retryTimer_.invalidate();
            sampleLevels();
            sampleMeasurements();
        }
    });
    connect(&engine, &AudioEngine::initializedChanged, this, [this](bool initialized) {
        if (!initialized) {
            source_.stop();
            resetLevels();
        }
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &MeterPanel::updatePresentation);
    connect(&FontManager::instance(), &FontManager::fontChanged, this, &MeterPanel::updatePresentation);
    setBus(0);
    setMinimumSize(minimumSizeHint());
}

QSize MeterPanel::sizeHint() const
{
    if (presentation_ == Presentation::Embedded) {
        const int preferredWidth = expandedWidth_ > 0 ? expandedWidth_ : std::max(208, fullPeakWidth() + 8);
        return {expandButton_->isChecked() ? std::max(minimumSizeHint().width(), preferredWidth)
            : minimumSizeHint().width(), 320};
    }
    return {240, 320};
}

void MeterPanel::setEmbeddedPreferredWidth(int width)
{
    expandedWidth_ = width;
    resize(sizeHint().width(), height());
}

QSize MeterPanel::minimumSizeHint() const
{
    if (presentation_ == Presentation::Standalone)
        return {160, 220};
    if (expandButton_->isChecked())
        return {100, 220};
    const int readoutWidth = QFontMetrics(FontManager::instance().getFont(FontRole::Axis))
        .horizontalAdvance(QStringLiteral("-96.0"));
    const auto margins = layout()->contentsMargins();
    return {std::max(header_->minimumSizeHint().width(), readoutWidth)
        + margins.left() + margins.right(), 220};
}

int MeterPanel::fullPeakWidth() const
{
    const auto& style = ThemeManager::instance().snapshot()->domains.meter.peakMeter;
    const QFontMetrics axis(FontManager::instance().getFont(FontRole::Axis));
    const int scaleWidth = std::max({30, style.metrics.outerInsetLeft,
        axis.horizontalAdvance(QStringLiteral("-96.0")) + 10});
    const int barWidth = QFontMetrics(peakValueFont()).horizontalAdvance(QStringLiteral("-96.0")) + 8;
    return scaleWidth + 2 * barWidth + style.metrics.reservedWidth
        + std::max(10, style.metrics.outerInsetRight) + 2;
}

PeakMeterLayoutMode MeterPanel::peakLayoutMode() const
{
    if (presentation_ == Presentation::Embedded && (!expandButton_ || !expandButton_->isChecked()))
        return PeakMeterLayoutMode::Inline;
    return isLevelMode() && canvas_ && canvas_->width() < 60
        ? PeakMeterLayoutMode::Inline : PeakMeterLayoutMode::Axis;
}

void MeterPanel::updatePresentation()
{
    const bool compact = presentation_ == Presentation::Embedded && !expandButton_->isChecked();
    modeSelector_->setVisible(presentation_ == Presentation::Standalone);
    busSelector_->setVisible(!compact && ProjectManager::instance().findBusItem(busSelector_->currentObjectID()));
    masterLabel_->setVisible(!compact && busSelector_->isHidden());
    if (presentation_ == Presentation::Embedded) {
        auto& theme = ThemeManager::instance();
        const auto& metrics = theme.metrics();
        for (auto* button : {expandButton_, detachButton_})
            button->setFixedSize(metrics.controlHeight, metrics.controlHeight);
        expandButton_->setIconSize(QSize(metrics.navigationChevronExtent, metrics.navigationChevronExtent));
        detachButton_->setIconSize(QSize(metrics.objectIconExtent, metrics.objectIconExtent));
        detachButton_->setVisible(!compact);
        header_->layout()->setAlignment(expandButton_, compact ? Qt::AlignHCenter : Qt::AlignLeft);
        if (compact)
            setFixedWidth(minimumSizeHint().width());
        else {
            setMaximumWidth(QWIDGETSIZE_MAX);
            setMinimumWidth(minimumSizeHint().width());
        }
        expandButton_->setIcon(theme.resolveToggleIcon(
            QString::fromLatin1(ThemeAssetIds::NavigationChevronDown),
            QString::fromLatin1(ThemeAssetIds::NavigationChevronRight)).icon);
        detachButton_->setIcon(theme.resolveIcon(QString::fromLatin1(ThemeAssetIds::InspectorDetach)).icon);
        expandButton_->setToolTip(compact ? tr("Expand meter") : tr("Collapse meter"));
        expandButton_->setAccessibleName(expandButton_->toolTip());
    }
    hoverPosition_.reset();
    update();
    updateDisplay();
}

QVariantMap MeterPanel::saveState() const
{
    QVariantMap state;
    if (const auto* bus = ProjectManager::instance().findBusItem(busObjectID_))
        state["busGuid"] = bus->guid.toString(QUuid::WithoutBraces);
    state["mode"] = static_cast<int>(mode_);
    state["loudnessMeasurement"] = static_cast<int>(loudnessMeasurement_);
    state["peakAmplitudeExponent"] = peakAmplitudeExponent_;
    state["spectrumAmplitudeExponent"] = spectrumAmplitudeExponent_;
    state["spectrogramSpeed"] = spectrogramSpeed_;
    return state;
}

void MeterPanel::restoreState(const QVariantMap& state)
{
    const auto* bus = ProjectManager::instance().findItemByGuid(QUuid::fromString(state.value("busGuid").toString()));
    setBus(bus && bus->type == ProjectItem::Type::Bus ? bus->objectID : 0);
    const auto exponent = [&state](const char* key) {
        const double value = state.value(key, 0.3).toDouble();
        return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.3;
    };
    peakAmplitudeExponent_ = exponent("peakAmplitudeExponent");
    spectrumAmplitudeExponent_ = exponent("spectrumAmplitudeExponent");
    setLoudnessMeasurement(static_cast<LoudnessMeasurement>(state.value("loudnessMeasurement", 0).toInt()));
    setSpectrogramSpeed(state.value("spectrogramSpeed", 2.0).toDouble());
    setMode(static_cast<Mode>(state.value("mode", 0).toInt()));
}

void MeterPanel::setBus(uint32_t objectID)
{
    auto& projects = ProjectManager::instance();
    auto* bus = projects.findBusItem(objectID);
    if (objectID != 0 && (!bus || bus->type != ProjectItem::Type::Bus))
        return;
    setDisplayPaused(false);
    busObjectID_ = bus && bus != projects.getMasterBusItem() ? bus->objectID : 0;
    busRuntimeId_ = busObjectID_ != 0 ? projects.busRuntimeHandle(bus) : harkenstudio::ObjectId{};
    busSelector_->setCurrentObjectID(bus ? bus->objectID : 0);
    updatePresentation();
    resetLevels();
    sampleLevels();
    sampleMeasurements();
}

void MeterPanel::setMode(Mode mode)
{
    const int index = modeSelector_->findData(static_cast<int>(mode));
    if (index < 0 || mode_ == mode || (presentation_ == Presentation::Embedded
            && mode != Mode::Peak && mode != Mode::Loudness))
        return;
    setDisplayPaused(false);
    const bool keepSpectrum = !isLevelMode() && (mode == Mode::Spectrum || mode == Mode::Spectrogram);
    mode_ = mode;
    spectrogramSpeedControl_->setVisible(mode_ == Mode::Spectrogram);
    hoverPosition_.reset();
    const QSignalBlocker blocker(modeSelector_);
    modeSelector_->setCurrentIndex(index);
    if (!keepSpectrum) {
        if (mode_ == Mode::Peak)
            source_.stop();
        resetMeasurements();
    }
    sampleMeasurements();
    updateDisplay();
}

void MeterPanel::setLoudnessMeasurement(LoudnessMeasurement measurement)
{
    if ((measurement != LoudnessMeasurement::Rms && measurement != LoudnessMeasurement::Lufs)
        || loudnessMeasurement_ == measurement)
        return;
    loudnessMeasurement_ = measurement;
    if (mode_ == Mode::Loudness) {
        resetMeasurements();
        sampleMeasurements();
        updateDisplay();
    }
}

double MeterPanel::amplitudeExponent() const
{
    return isLevelMode() ? peakAmplitudeExponent_ : spectrumAmplitudeExponent_;
}

void MeterPanel::setAmplitudeExponent(double exponent)
{
    if (mode_ == Mode::Spectrogram || !std::isfinite(exponent))
        return;
    auto& current = isLevelMode() ? peakAmplitudeExponent_ : spectrumAmplitudeExponent_;
    exponent = std::clamp(exponent, 0.0, 1.0);
    if (current == exponent)
        return;
    current = exponent;
    canvas_->update();
}

void MeterPanel::setSpectrogramSpeed(double speed)
{
    if (!std::isfinite(speed))
        return;
    spectrogramSpeed_ = std::round(std::clamp(speed, kSpectrogramMinSpeed, kSpectrogramMaxSpeed) * 100.0) / 100.0;
    if (!qFuzzyCompare(spectrogramSpeedControl_->value(), spectrogramSpeed_))
        spectrogramSpeedControl_->setValue(spectrogramSpeed_);
    canvas_->update();
}

AmplitudeScale MeterPanel::amplitudeAxis() const
{
    const double maximum = isLevelMode() ? MeterScale::kPeakMaxDb
        : MeterScale::kSpectrumMaxDb;
    return {MeterScale::kMinDb, maximum, amplitudeExponent()};
}

void MeterPanel::sampleLevels()
{
    if (!isVisible())
        return;
    const auto& engine = AudioEngine::instance();
    if (!engine.isInitialized() || !engine.isPlaying())
        return;
    float left = 0.0f;
    float right = 0.0f;
    if (busObjectID_ == 0) {
        engine.getMasterMeterPeak(left, right);
    } else if (engine.getBusMeterPeakById(busRuntimeId_, left, right)
               != HarkenEditor::Bridge::IpcQueryStatus::Ok) {
        if (liveDisplay_.levelAvailable)
            resetLevels();
        return;
    }
    acceptPeak(left, right);
}

void MeterPanel::acceptPeak(float left, float right)
{
    if (!isVisible() || !std::isfinite(left) || !std::isfinite(right) || left < 0.0f || right < 0.0f)
        return;
    advanceBallistics();
    const std::array peaks{left, right};
    for (int channel = 0; channel < 2; ++channel) {
        const float peak = peaks[channel];
        const float peakDb = peak > 0.0f
            ? std::clamp(20.0f * std::log10(peak), MeterScale::kMinDb, MeterScale::kPeakMaxDb)
            : MeterScale::kMinDb;
        liveDisplay_.peakBallistics[channel].advance(peakDb, 0.0f);
        liveDisplay_.clipped[channel] = liveDisplay_.clipped[channel] || peak >= 1.0f;
    }
    liveDisplay_.levelAvailable = true;
}

void MeterPanel::resetLevels()
{
    resetMeasurements();
    liveDisplay_.peakBallistics = {};
    liveDisplay_.clipped = {};
    liveDisplay_.levelAvailable = false;
    updateDisplay();
}

void MeterPanel::setDisplayPaused(bool paused)
{
    if (paused == pausedDisplay_.has_value())
        return;
    if (paused)
        pausedDisplay_ = liveDisplay_;
    else
        pausedDisplay_.reset();
    canvas_->update();
}

void MeterPanel::resetMeasurements()
{
    liveDisplay_.measurementAvailable = false;
    liveDisplay_.loudnessBallistics = {};
    liveDisplay_.spectrumLevels.fill(0.0f);
    liveDisplay_.spectrumPeaks.fill(0.0f);
    spectrumBallistics_.fill({});
    liveDisplay_.spectrogram = {};
    retryTimer_.invalidate();
}

void MeterPanel::sampleMeasurements()
{
    const auto& engine = AudioEngine::instance();
    if (mode_ == Mode::Peak || !isVisible() || !engine.isInitialized() || !engine.isPlaying())
        return;
    if (!source_.active()) {
        if (source_.result() != Monitor::kOk && retryTimer_.isValid() && retryTimer_.elapsed() < 1000)
            return;
        retryTimer_.start();
    }
    const auto bus = busObjectID_ == 0 ? HarkenEditor::Bridge::kEngineMasterBusId : busRuntimeId_;
    const auto measurement = mode_ != Mode::Loudness ? HarkenEditor::Bridge::MeterMeasurement::Spectrum
        : loudnessMeasurement_ == LoudnessMeasurement::Rms ? HarkenEditor::Bridge::MeterMeasurement::Rms
        : HarkenEditor::Bridge::MeterMeasurement::LufsMomentary;
    if (!source_.start(bus, measurement))
        return;
    auto samples = source_.poll();
    if (samples.interrupted)
        resetMeasurements();
    for (const auto& frame : samples.frames)
        acceptMeasurement(frame);
}

void MeterPanel::acceptMeasurement(const HarkenEditor::Bridge::MeterMeasurementFrame& frame)
{
    if (mode_ == Mode::Loudness) {
        float db = -std::numeric_limits<float>::infinity();
        if (loudnessMeasurement_ == LoudnessMeasurement::Rms) {
            if (!frame.rmsAvailable || !std::isfinite(frame.rms) || frame.rms < 0.0f)
                return;
            if (frame.rms > 0.0f)
                db = 20.0f * std::log10(frame.rms);
        } else {
            if (!frame.lufsAvailable || std::isnan(frame.lufs)
                || frame.lufs == std::numeric_limits<float>::infinity())
                return;
            db = frame.lufs;
        }
        advanceBallistics();
        if (!liveDisplay_.measurementAvailable)
            liveDisplay_.loudnessBallistics = {db, db, 0.0f};
        liveDisplay_.loudnessBallistics.advance(db, 0.0f);
        liveDisplay_.measurementAvailable = true;
        return;
    }
    if (frame.rmsAvailable || frame.lufsAvailable || mode_ == Mode::Peak)
        return;
    if (frame.sampleRate < 1000 || frame.fftSize != 2048
        || frame.spectrumDb.size() != frame.fftSize / 2 + 1
        || !std::all_of(frame.spectrumDb.begin(), frame.spectrumDb.end(),
                       [](float db) { return std::isfinite(db); }))
        return;
    advanceBallistics();
    if (liveDisplay_.spectrumSampleRate != frame.sampleRate)
        resetMeasurements();
    liveDisplay_.spectrumSampleRate = frame.sampleRate;
    const float maxFrequency = std::min(20000.0f, frame.sampleRate * 0.5f);
    const float binWidth = static_cast<float>(frame.sampleRate) / frame.fftSize;
    if (liveDisplay_.spectrogram.capacity() == 0) {
        const int columns = static_cast<int>(std::ceil(kSpectrogramHistorySeconds
            * frame.sampleRate / (frame.fftSize / 2)));
        liveDisplay_.spectrogram = SpectrogramHistory(frame.sampleRate, frame.fftSize, columns);
    }
    for (int point = 0; point < kSpectrumMeterDisplayPoints; ++point) {
        const float norm = static_cast<float>(point) / (kSpectrumMeterDisplayPoints - 1);
        const float frequency = FrequencyScale::normToFreq(norm, MeterScale::kMinFrequency, maxFrequency, FreqScaleType::Log);
        const float nextFrequency = FrequencyScale::normToFreq(
            std::min(1.0f, norm + 1.0f / (kSpectrumMeterDisplayPoints - 1)),
            MeterScale::kMinFrequency, maxFrequency, FreqScaleType::Log);
        const int bin = std::clamp(static_cast<int>(frequency / binWidth), 0,
                                  static_cast<int>(frame.spectrumDb.size()) - 2);
        const float fraction = frequency / binWidth - bin;
        float db = std::lerp(frame.spectrumDb[bin], frame.spectrumDb[bin + 1], fraction);
        const int last = std::min(static_cast<int>(nextFrequency / binWidth),
                                  static_cast<int>(frame.spectrumDb.size()) - 1);
        for (int index = bin + 1; index <= last; ++index)
            db = std::max(db, frame.spectrumDb[index]);
        const float inputDb = std::clamp(db, MeterScale::kMinDb, MeterScale::kSpectrogramMaxDb);
        spectrumBallistics_[point].advance(inputDb, 0.0f);
    }
    liveDisplay_.spectrogram.append(frame.spectrumDb);
    liveDisplay_.measurementAvailable = true;
    updateSpectrumLevels();
}

void MeterPanel::advanceBallistics()
{
    const float seconds = static_cast<float>(elapsed_.nsecsElapsed() / 1.0e9);
    elapsed_.restart();
    for (auto& ballistics : liveDisplay_.peakBallistics)
        ballistics.advance(ballistics.levelDb, seconds);
    if (mode_ == Mode::Loudness && liveDisplay_.measurementAvailable)
        liveDisplay_.loudnessBallistics.advance(liveDisplay_.loudnessBallistics.levelDb, seconds);
    if (!isLevelMode() && liveDisplay_.measurementAvailable) {
        for (auto& ballistics : spectrumBallistics_)
            ballistics.advance(ballistics.levelDb, seconds);
    }
}

void MeterPanel::updateSpectrumLevels()
{
    const auto amplitude = [](float db) {
        return db <= MeterScale::kMinDb ? 0.0f : std::pow(10.0f, db / 20.0f);
    };
    for (int point = 0; point < kSpectrumMeterDisplayPoints; ++point) {
        liveDisplay_.spectrumLevels[point] = amplitude(spectrumBallistics_[point].levelDb);
        liveDisplay_.spectrumPeaks[point] = amplitude(spectrumBallistics_[point].peakDb);
    }
}

void MeterPanel::resetPeakHold()
{
    for (auto& ballistics : liveDisplay_.peakBallistics) {
        ballistics.peakDb = ballistics.levelDb;
        ballistics.holdSeconds = 0.0f;
    }
    liveDisplay_.loudnessBallistics.peakDb = liveDisplay_.loudnessBallistics.levelDb;
    liveDisplay_.loudnessBallistics.holdSeconds = 0.0f;
    liveDisplay_.clipped = {};
    updateDisplay();
}

void MeterPanel::refresh()
{
    advanceBallistics();
    sampleMeasurements();
    if (!isLevelMode() && liveDisplay_.measurementAvailable)
        updateSpectrumLevels();
    updateDisplay();
}

void MeterPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    resetLevels();
    sampleLevels();
    elapsed_.start();
    refreshTimer_->start();
    refresh();
}

void MeterPanel::hideEvent(QHideEvent* event)
{
    setDisplayPaused(false);
    hoverPosition_.reset();
    source_.stop();
    refreshTimer_->stop();
    resetLevels();
    QWidget::hideEvent(event);
}

bool MeterPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == header_ && event->type() == QEvent::LayoutRequest)
        updateDisplay();
    if (watched != canvas_)
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::Paint: {
        QPainter painter(canvas_);
        paintCanvas(painter);
        return true;
    }
    case QEvent::Resize:
        layoutSpectrogramSpeedControl();
        break;
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (pausedDisplay_ && !mouse->buttons().testFlag(Qt::LeftButton))
            setDisplayPaused(false);
        hoverPosition_ = mouse->position();
        canvas_->update();
        break;
    }
    case QEvent::Leave:
        hoverPosition_.reset();
        canvas_->update();
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && plotBounds().contains(mouse->position().toPoint())) {
            if (event->type() == QEvent::MouseButtonDblClick && presentation_ == Presentation::Embedded) {
                emit detachRequested(saveState());
                return true;
            }
            if (!isLevelMode()) {
                setDisplayPaused(true);
                return true;
            }
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton && pausedDisplay_) {
            setDisplayPaused(false);
            return true;
        }
        break;
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
        setDisplayPaused(false);
        break;
    case QEvent::ContextMenu: {
        const auto* context = static_cast<QContextMenuEvent*>(event);
        const QRect plot = plotBounds();
        const bool inlineScale = peakLayoutMode() == PeakMeterLayoutMode::Inline;
        const bool selectMeasurement = mode_ == Mode::Loudness || presentation_ == Presentation::Embedded;
        const Mode mode = mode_;
        const auto changeExponent = [this, mode](double exponent) {
            if (mode_ == mode)
                setAmplitudeExponent(exponent);
        };
        if (plot.contains(context->pos()) && (inlineScale || selectMeasurement)) {
            auto* menu = new QMenu(canvas_);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            const auto addMeasurement = [&](const QString& text, Mode mode, LoudnessMeasurement measurement) {
                auto* action = menu->addAction(text);
                action->setCheckable(true);
                action->setChecked(mode_ == mode && (mode == Mode::Peak || loudnessMeasurement_ == measurement));
                connect(action, &QAction::triggered, this, [this, mode, measurement] {
                    if (mode == Mode::Loudness)
                        setLoudnessMeasurement(measurement);
                    setMode(mode);
                });
            };
            if (selectMeasurement) {
                if (presentation_ == Presentation::Embedded)
                    addMeasurement(tr("Peak"), Mode::Peak, loudnessMeasurement_);
                addMeasurement(tr("RMS"), Mode::Loudness, LoudnessMeasurement::Rms);
                addMeasurement(tr("LUFS"), Mode::Loudness, LoudnessMeasurement::Lufs);
            }
            if (inlineScale) {
                if (selectMeasurement)
                    menu->addSeparator();
                addAmplitudePowerLawAction(menu, canvas_, context->globalPos(),
                    QRect(canvas_->mapToGlobal(plot.topLeft()), plot.size()), amplitudeExponent(), changeExponent);
            }
            menu->popup(context->globalPos());
            return true;
        }
        const QRect axis(1, plot.top(), plot.left() - 1, plot.height());
        if (!inlineScale && mode_ != Mode::Spectrogram && axis.contains(context->pos())) {
            showAmplitudeCurveMenu(canvas_, context->globalPos(),
                QRect(canvas_->mapToGlobal(axis.topLeft()), axis.size()), amplitudeExponent(), changeExponent);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void MeterPanel::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (presentation_ == Presentation::Embedded) {
        QPainter painter(this);
        painter.setPen(ThemeManager::instance().snapshot()->ui.colors.viewBorder);
        painter.drawLine(width() - 1, 0, width() - 1, height() - 1);
        painter.drawLine(0, header_->geometry().bottom(), width() - 1, header_->geometry().bottom());
    }
}

QRect MeterPanel::canvasBounds() const
{
    return presentation_ == Presentation::Embedded
        ? canvas_->rect() : canvas_->rect().adjusted(1, 1, -1, -1);
}

PeakMeterLayout MeterPanel::peakLayout() const
{
    const auto& style = ThemeManager::instance().snapshot()->domains.meter.peakMeter;
    return PeakMeterRenderer::layout(canvasBounds(), style, mode_ == Mode::Loudness ? 1 : 2, peakLayoutMode());
}

QRect MeterPanel::plotBounds() const
{
    const auto& style = ThemeManager::instance().snapshot()->domains.meter;
    const QRect bounds = canvasBounds();
    if (isLevelMode())
        return peakLayout().meterBounds;
    return mode_ == Mode::Spectrum
        ? SpectrumMeterRenderer::layout(bounds, style.spectrumMeter).plotBounds
        : SpectrogramMeterRenderer::layout(bounds, style.spectrogramMeter).plotBounds;
}

AxisCrosshair MeterPanel::crosshair() const
{
    AxisCrosshair result;
    const QRect plot = plotBounds();
    if (!hoverPosition_ || !plot.isValid() || !QRectF(plot).contains(*hoverPosition_))
        return result;
    result.visible = true;
    result.position = *hoverPosition_;
    const float x = std::clamp(float((hoverPosition_->x() - plot.left()) / std::max(1, plot.width() - 1)), 0.0f, 1.0f);
    const float y = std::clamp(float((plot.bottom() - hoverPosition_->y()) / std::max(1, plot.height() - 1)), 0.0f, 1.0f);
    if (isLevelMode()) {
        result.label = formatAxisValue(amplitudeAxis().positionToDb(y), 1);
        return result;
    }
    const float maximum = std::min(20000.0f, displayState().spectrumSampleRate * 0.5f);
    const float frequency = FrequencyScale::normToFreq(mode_ == Mode::Spectrum ? x : y,
        MeterScale::kMinFrequency, maximum, FreqScaleType::Log);
    const QString frequencyText = frequency >= 1000.0f
        ? formatAxisValue(frequency / 1000.0, 2) + QStringLiteral(" kHz")
        : formatAxisValue(frequency, 1) + QStringLiteral(" Hz");
    if (mode_ == Mode::Spectrum) {
        result.label = QStringLiteral("%1 | %2 dBFS").arg(frequencyText,
            formatAxisValue(amplitudeAxis().positionToDb(y), 1));
    } else {
        result.label = frequencyText;
    }
    return result;
}

void MeterPanel::layoutSpectrogramSpeedControl()
{
    if (mode_ != Mode::Spectrogram)
        return;
    const QRect plot = plotBounds();
    const auto& metrics = ThemeManager::instance().snapshot()->domains.meter.spectrogramMeter.metrics;
    const int top = plot.bottom() + qRound(metrics.timeLabelTopOffset);
    spectrogramSpeedControl_->setGeometry(plot.left(), top, plot.width(), canvas_->height() - top - 2);
}

void MeterPanel::updateDisplay()
{
    masterLabel_->setFixedHeight(modeSelector_->sizeHint().height());
    if (header_) {
        const auto margins = header_->layout()->contentsMargins() + header_->contentsMargins();
        const int height = std::max({expandButton_->height(), masterLabel_->height(),
            busSelector_->sizeHint().expandedTo(busSelector_->minimumSize()).height()})
            + margins.top() + margins.bottom();
        if (header_->minimumHeight() != height) {
            header_->setFixedHeight(height);
            emit embeddedHeaderHeightChanged();
        }
    }
    layoutSpectrogramSpeedControl();
    QString tooltip;
    if (mode_ == Mode::Peak)
        tooltip = tr("Stereo bus peak (dBFS).");
    else if (mode_ == Mode::Loudness)
        tooltip = loudnessMeasurement_ == LoudnessMeasurement::Rms
            ? tr("RMS (dBFS), 300 ms.")
            : tr("LUFS, 400 ms.");
    else
        tooltip = tr("Mono-average spectrum, 2048-point Hann FFT. Hold the left mouse button to pause.");
    if (presentation_ == Presentation::Embedded)
        tooltip += tr(" Double-click to open a new meter. Right-click to select Peak, RMS or LUFS.");
    else if (mode_ == Mode::Loudness)
        tooltip += tr(" Right-click to select RMS or LUFS.");
    canvas_->setToolTip(tooltip);
    canvas_->update();
}

void MeterPanel::paintCanvas(QPainter& painter)
{
    auto& theme = ThemeManager::instance();
    const auto snapshot = theme.snapshot();
    const auto& style = snapshot->domains.meter;
    const auto& curve = snapshot->domains.curve;
    const QRect canvas = canvasBounds();
    const QRect plot = plotBounds();
    const QFont axisFont = FontManager::instance().getFont(FontRole::Axis);
    const QFontMetrics font(axisFont);
    painter.save();
    painter.setClipRect(canvas);
    if (isLevelMode()) {
        const auto geometry = peakLayout();
        const bool compact = peakLayoutMode() != PeakMeterLayoutMode::Axis;
        const QRect scale(canvas.left(), canvas.top(), plot.left() - canvas.left(), canvas.height());
        PeakMeterRenderFrame frame;
        frame.layoutMode = peakLayoutMode();
        frame.canvasBounds = canvas;
        frame.scaleBounds = scale;
        frame.barCount = plot.isValid() ? (mode_ == Mode::Loudness ? 1 : 2) : 0;
        frame.labelFont = FontManager::instance().getFont(FontRole::Label);
        frame.peakCap = theme.resolveImage(QString::fromLatin1(ThemeAssetIds::MeterPeakCap),
            {{}, devicePixelRatioF(), 0, {}}).resource;
        for (const auto& tick : buildAmplitudeAxisTicks(plot, plot.bottom(), plot.height() - 1,
                font, curve.metrics.amplitudeGridTargetRowSpacing, amplitudeAxis(), false))
            frame.ticks.append({qreal(tick.y), tick.label, tick.major});
        for (int channel = 0; channel < frame.barCount; ++channel) {
            auto& bar = frame.bars[channel];
            bar.bounds = geometry.barBounds[channel];
            bar.amplitudeAxis = amplitudeAxis();
            const auto yForDb = [&](float db) {
                return plot.bottom() - bar.amplitudeAxis.dbToPosition(db) * (plot.height() - 1);
            };
            const float db = mode_ == Mode::Loudness ? loudnessDb() : levelDb(channel);
            bar.showLevel = db > MeterScale::kMinDb;
            bar.levelTop = yForDb(db);
            const float heldDb = mode_ == Mode::Loudness ? heldLoudnessDb() : heldPeakDb(channel);
            bar.showHold = heldDb > MeterScale::kMinDb;
            bar.holdY = yForDb(heldDb);
        }
        PeakMeterRenderer::render(painter, style, frame);
        const auto cursor = crosshair();
        if (cursor.visible) {
            drawAxisCrosshairLines(painter, curve,
                QRectF(QPointF(plot.topLeft()), QPointF(plot.bottomRight())),
                cursor.position, Qt::Horizontal);
            const qreal labelHeight = std::min(qreal(canvas.height()),
                std::max(curve.metrics.readoutHeight, qreal(font.height() + 4)));
            const qreal labelWidth = compact
                ? std::min(qreal(plot.width()), font.horizontalAdvance(cursor.label) + curve.metrics.readoutHorizontalPadding)
                : scale.width();
            qreal top = cursor.position.y() - (compact ? labelHeight + 1.0 : labelHeight / 2.0);
            if (compact && top < plot.top())
                top = cursor.position.y() + 1.0;
            top = std::clamp(top, qreal(canvas.top()), qreal(canvas.bottom() + 1) - labelHeight);
            const QRectF readout(compact ? plot.left() + (plot.width() - labelWidth) / 2.0 : scale.left(),
                top, labelWidth, labelHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(curve.colors.readoutBackground);
            painter.drawRoundedRect(readout, curve.metrics.readoutRadius, curve.metrics.readoutRadius);
            painter.setFont(axisFont);
            painter.setPen(curve.colors.readoutText);
            painter.drawText(compact ? readout : readout.adjusted(0, 0, -6, 0),
                compact ? Qt::AlignCenter : Qt::AlignRight | Qt::AlignVCenter, cursor.label);
        }
        const QFont valueFont = peakValueFont();
        const int valueWidth = QFontMetrics(valueFont).horizontalAdvance(QStringLiteral("-96.0"));
        for (int channel = 0; channel < frame.barCount; ++channel) {
            const auto& bar = frame.bars[channel];
            if (bar.bounds.width() < valueWidth)
                continue;
            QPainterPath value;
            const float db = mode_ == Mode::Loudness ? loudnessDb() : heldPeakDb(channel);
            value.addText(0, 0, valueFont,
                std::isinf(db) ? QStringLiteral("-\u221e") : formatAxisValue(db, 1));
            const QRectF valueBounds = value.boundingRect();
            if (valueBounds.width() > bar.bounds.width())
                continue;
            value.translate(bar.bounds.left() + (bar.bounds.width() - valueBounds.width()) / 2.0 - valueBounds.left(),
                plot.bottom() - 12 - valueBounds.bottom());
            painter.save();
            painter.setClipRect(bar.bounds, Qt::IntersectClip);
            painter.setRenderHint(QPainter::Antialiasing);
            if (mode_ == Mode::Loudness ? measurementAvailable() : (levelAvailable() && bar.showHold))
                painter.fillPath(value, snapshot->ui.colors.accentText);
            painter.restore();
        }
    } else {
        paintMeasurement(painter, canvas);
        drawAxisCrosshair(painter, curve,
            QRectF(QPointF(plot.topLeft()), QPointF(plot.bottomRight())), crosshair());
    }
    painter.restore();
    painter.setPen(snapshot->ui.colors.viewBorder);
    painter.setBrush(Qt::NoBrush);
    if (presentation_ == Presentation::Standalone)
        painter.drawRect(canvas_->rect().adjusted(0, 0, -1, -1));
}

void MeterPanel::paintMeasurement(QPainter& painter, const QRect& canvas)
{
    const auto& display = displayState();
    const auto snapshot = ThemeManager::instance().snapshot();
    const auto& style = snapshot->domains.meter;
    const QFontMetrics font(FontManager::instance().getFont(FontRole::Axis));
    const QRect plot = plotBounds();
    const float maximum = std::min(20000.0f, display.spectrumSampleRate * 0.5f);
    if (mode_ == Mode::Spectrum) {
        SpectrumMeterRenderFrame frame;
        frame.canvasBounds = canvas;
        frame.plotBounds = plot;
        frame.hasPlot = plot.isValid();
        frame.amplitudeAxis = amplitudeAxis();
        frame.smoothedLevels = display.spectrumLevels;
        frame.peakLevels = display.spectrumPeaks;
        for (const auto& tick : buildAmplitudeAxisTicks(plot, plot.bottom(), plot.height() - 1,
                font, snapshot->domains.curve.metrics.amplitudeGridTargetRowSpacing,
                frame.amplitudeAxis, false))
            frame.dbTicks.append({qreal(tick.y), tick.label, tick.major});
        frame.frequencyTicks = MeterScale::frequencyTicks(maximum, plot.width(), font, false);
        SpectrumMeterRenderer::render(painter, style, frame);
    } else {
        SpectrogramMeterRenderFrame frame;
        frame.canvasBounds = canvas;
        frame.plotBounds = plot;
        frame.hasPlot = plot.isValid();
        const qreal pixelRatio = painter.device()->devicePixelRatioF();
        const double columnsPerPixel = display.spectrumSampleRate
            / (1024.0 * kSpectrogramPixelsPerSecond * spectrogramSpeed_ * pixelRatio);
        frame.waterfallImage = display.spectrogram.image(
            QSize(qRound(plot.width() * pixelRatio), qRound(plot.height() * pixelRatio)),
            columnsPerPixel, MeterColorMap::colorTable(style.spectrogramMeter.colors));
        for (const auto& tick : MeterScale::frequencyTicks(maximum, plot.height(), font, true))
            frame.frequencyTicks.append({plot.bottom() - tick.normalizedPosition * (plot.height() - 1), tick.text, tick.major});
        SpectrogramMeterRenderer::render(painter, style, frame);
    }
}

}
